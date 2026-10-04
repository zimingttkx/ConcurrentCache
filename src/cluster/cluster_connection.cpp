// cluster_connection.cpp
#include "cluster_connection.h"
#include "cluster_server.h"
#include "cluster_gossip.h"
#include "replication_mgr.h"
#include "base/log.h"
#include "protocol/resp.h"
#include <chrono>

// 获取当前时间的毫秒数
static int64_t get_current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

namespace cc_server {

ClusterConnection::ClusterConnection()
    : heartbeat_interval_ms_(1000),
      ping_timeout_ms_(5000),
      last_heartbeat_time_ms_(0),
      heartbeat_running_(false) {
    LOG_INFO(CLUSTER, "ClusterConnection created");
}

ClusterConnection::~ClusterConnection() {
    stop_heartbeat();
    disconnect_all();
    LOG_INFO(CLUSTER, "ClusterConnection destroyed");
}

void ClusterConnection::init() {
    state_ = ClusterServer::instance().getState();
    LOG_INFO(CLUSTER, "ClusterConnection initialized");
}

void ClusterConnection::start_heartbeat() {
    if (heartbeat_running_) {
        LOG_WARN(CLUSTER, "Heartbeat already running");
        return;
    }
    heartbeat_running_ = true;
    heartbeat_thread_stop_ = false;
    last_heartbeat_time_ms_ = get_current_time_ms();
    heartbeat_thread_ = std::thread([this]() {
        while (!heartbeat_thread_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval_ms_));
            if (!heartbeat_thread_stop_.load()) {
                on_timer();
                // 驱动故障转移状态机：执行 PFAIL→FAIL 检查与 failover 推进
                // （修复 P0-4：此前 executeFailover 从未被周期性调用，
                // 导致故障检测→failover 链路断裂）
                ClusterServer::instance().on_timer();
            }
        }
    });
    LOG_INFO(CLUSTER, "Heartbeat started (interval=%ldms, timeout=%ldms)",
             heartbeat_interval_ms_, ping_timeout_ms_);
}

void ClusterConnection::stop_heartbeat() {
    if (!heartbeat_running_) {
        return;
    }
    heartbeat_running_ = false;
    heartbeat_thread_stop_ = true;
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    LOG_INFO(CLUSTER, "Heartbeat stopped");
}

bool ClusterConnection::connect_to_node(const std::string& node_name,
                                       const std::string& ip, int port) {
    // 连接目标节点的集群总线端口（port + 10000），而非主端口
    int bus_port = port + 10000;
    // 先检查是否已存在连接
    {
        std::shared_lock<std::shared_mutex> lock(links_mutex_);
        if (links_.find(node_name) != links_.end()) {
            LOG_INFO(CLUSTER, "Already connected to node: %s", node_name.c_str());
            return true;
        }
    }

    // 创建新连接（连接目标节点的集群总线端口）
    auto link = std::make_unique<ClusterLink>(node_name, ip, bus_port);

    // 设置消息回调 - 处理收到的消息
    link->set_msg_callback([this](ClusterMsg&& msg, ClusterLink* cluster_link) {
        handle_link_msg(std::move(msg), cluster_link);
    });

    // 设置断开回调
    link->set_disconnect_callback([this](const std::string& name, ClusterLink* cluster_link) {
        on_node_disconnected(name, cluster_link);
    });

    // 建立连接
    if (!link->connect()) {
        LOG_ERROR(CLUSTER, "Failed to connect to node: %s (%s:%d)",
                 node_name.c_str(), ip.c_str(), port);
        return false;
    }

    // 获取原始指针（用于注册到 EventLoop）
    ClusterLink* raw_link = link.get();

    // 添加到连接列表
    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        links_[node_name] = std::shared_ptr<ClusterLink>(std::move(link));
    }

    // 注册到 EventLoop（如果有）
    if (event_loop_) {
        register_link_to_loop(raw_link);
    }

    LOG_INFO(CLUSTER, "Connected to node: %s (%s:%d)", node_name.c_str(), ip.c_str(), port);

    if (node_connected_callback_) {
        node_connected_callback_(node_name);
    }

    return true;
}

void ClusterConnection::disconnect_from_node(const std::string& node_name) {
    // 修复 P0-2：旧代码先 links_.erase(it)（析构 link），随后还解引用 link_ptr
    // 调用 fd()/disconnect() —— 直接 UAF。正确顺序：
    //   1) 取出 link 指针（link 仍存活）
    //   2) 注销 Channel + disconnect()
    //   3) 最后 erase（销毁 link）
    // 注意：普通 disconnect() 不触发断开回调，不会重入本函数。
    ClusterLink* link_ptr = nullptr;

    {
        std::shared_lock<std::shared_mutex> lock(links_mutex_);
        auto it = links_.find(node_name);
        if (it != links_.end()) {
            link_ptr = it->second.get();
        }
    }

    if (link_ptr) {
        // 先从 EventLoop 注销 Channel，再断开 socket（link 此时仍有效）
        if (event_loop_) {
            unregister_link_from_loop(link_ptr);
        }
        link_ptr->disconnect();

        // 最后从 map 移除并销毁 link
        {
            std::unique_lock<std::shared_mutex> lock(links_mutex_);
            links_.erase(node_name);
        }

        LOG_INFO(CLUSTER, "Disconnected from node: %s", node_name.c_str());

        if (node_disconnected_callback_) {
            node_disconnected_callback_(node_name);
        }
    }
}

void ClusterConnection::disconnect_all() {
    // 先收集所有 Link 指针并从 EventLoop 注销
    std::vector<ClusterLink*> links_to_disconnect;

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);

        for (auto& [name, link] : links_) {
            links_to_disconnect.push_back(link.get());
        }
    }

    // 在锁外注销所有 Channel
    if (event_loop_) {
        for (auto* link : links_to_disconnect) {
            unregister_link_from_loop(link);
        }
    }

    // 断开所有连接
    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);

        for (auto* link : links_to_disconnect) {
            link->disconnect();
        }
        links_.clear();
    }

    LOG_INFO(CLUSTER, "Disconnected from all nodes");
}

std::vector<std::shared_ptr<ClusterLink>> ClusterConnection::snapshot_links() const {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);
    std::vector<std::shared_ptr<ClusterLink>> out;
    out.reserve(links_.size());
    for (const auto& [name, link] : links_) {
        out.push_back(link);
    }
    return out;
}

std::shared_ptr<ClusterLink> ClusterConnection::find_link(const std::string& node_name) const {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);
    auto it = links_.find(node_name);
    return it != links_.end() ? it->second : std::shared_ptr<ClusterLink>();
}

ClusterLink* ClusterConnection::get_link(const std::string& node_name) {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    auto it = links_.find(node_name);
    if (it != links_.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::vector<ClusterLink*> ClusterConnection::get_all_links() {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    std::vector<ClusterLink*> result;
    result.reserve(links_.size());

    for (auto& [name, link] : links_) {
        result.push_back(link.get());
    }
    return result;
}

bool ClusterConnection::send_to_node(const std::string& node_name, const ClusterMsg& msg) {
    auto* link = get_link(node_name);
    if (!link) {
        LOG_WARN(CLUSTER, "No link to node: %s", node_name.c_str());
        return false;
    }
    return link->send_msg(msg);
}

bool ClusterConnection::ping_node(const std::string& node_name) {
    auto* link = get_link(node_name);
    if (!link) {
        return false;
    }
    return link->send_ping();
}

bool ClusterConnection::pong_node(const std::string& node_name) {
    auto* link = get_link(node_name);
    if (!link) {
        return false;
    }
    return link->send_pong();
}

bool ClusterConnection::meet_node(const std::string& node_name,
                                   const std::string& my_ip, int my_port) {
    auto* link = get_link(node_name);
    if (!link) {
        return false;
    }
    return link->send_meet(my_ip, my_port);
}

bool ClusterConnection::send_command_to_node(const std::string& node_name,
                                           const std::vector<std::string>& args) {
    // shared_ptr 副本而不是裸指针：发送可能触发断开回调并让表项被擦除（见 broadcast_*）
    auto link = find_link(node_name);
    if (!link) {
        LOG_WARN(CLUSTER, "No link to node for command: %s", node_name.c_str());
        return false;
    }

    // 将命令包装在 ClusterMsg 中（使用 kRepData 类型）。
    // 帧长不在这里算：send_msg() 会按 args 重新计算并校验上限，这里再算一遍就是
    // 第二处真相（而且它是 uint32 累加，参数够多时会回绕——正是 P0-1 的老形状）。
    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kRepData);
    msg.args = args;
    return link->send_msg(msg);
}

bool ClusterConnection::send_raw_to_node(const std::string& node_name,
                                         const std::string& data) {
    auto link = find_link(node_name);
    if (!link) {
        LOG_WARN(CLUSTER, "No link to node for raw data: %s", node_name.c_str());
        return false;
    }
    return link->send_raw(data);
}

void ClusterConnection::broadcast_ping() {
    // 锁内只抄指针，发送一律在锁外做。原来是在 shared_lock 里直接 send_*：某条链路
    // 一旦写失败，ClusterLink::disconnect_and_notify 会回调 on_node_disconnected，
    // 那里要拿 links_mutex_ 的**独占**锁；std::shared_mutex 既不可重入也不支持
    // 锁升级，同一线程持有共享锁再要独占锁就永久停在这里 —— 而这个线程往往是
    // SubReactor 或总线线程，于是整台服务器不再应答任何客户端。
    for (auto& link : snapshot_links()) {
        if (link->is_connected()) {
            link->send_ping();
        }
    }
}

void ClusterConnection::broadcast_pong() {
    for (auto& link : snapshot_links()) {
        if (link->is_connected()) {
            link->send_pong();
        }
    }
}

void ClusterConnection::broadcast_gossip(const GossipMsg& msg) {
    // 同 broadcast_ping：不能在持有 links_mutex_ 共享锁时发送。这条路是
    // CLUSTER FAIL 与故障广播的必经路径，一次对端写失败就能把调用它的线程
    // （通常是处理该客户端命令的 SubReactor）永久挂住。
    for (auto& link : snapshot_links()) {
        if (link->is_connected()) {
            link->send_gossip(msg);
        }
    }
}

size_t ClusterConnection::connected_count() const {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    size_t count = 0;
    for (const auto& [name, link] : links_) {
        if (link->is_connected()) {
            count++;
        }
    }
    return count;
}

bool ClusterConnection::is_node_connected(const std::string& node_name) const {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    auto it = links_.find(node_name);
    if (it != links_.end()) {
        return it->second->is_connected();
    }
    return false;
}

void ClusterConnection::on_node_disconnected(const std::string& node_name, ClusterLink* link) {
    // 修复 P0-2：link 可能为 nullptr —— ClusterBus 侧的断开回调里 link 即将/已经
    // 被销毁（其 Channel 由 ClusterBus::remove_link 负责注销），传 nullptr 表示
    // "没有可用的 link 对象"。只有传入仍有效的 link 时才做 EventLoop 注销。
    if (link && event_loop_) {
        unregister_link_from_loop(link);
    }

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        links_.erase(node_name);
    }

    LOG_INFO(CLUSTER, "Node disconnected: %s", node_name.c_str());

    if (node_disconnected_callback_) {
        node_disconnected_callback_(node_name);
    }
}

void ClusterConnection::register_link_to_loop(ClusterLink* link) {
    if (!event_loop_ || !link) {
        return;
    }

    int fd = link->fd();
    if (fd < 0) {
        return;
    }

    // 创建 Channel
    auto* channel = new Channel(event_loop_, fd);

    // 设置回调：当 fd 可读时调用 handle_read
    channel->set_read_callback([link]() {
        link->handle_read();
    });

    // 设置回调：当 fd 可写时调用 handle_write（用于发送缓冲区数据）
    channel->set_write_callback([link]() {
        link->handle_write();
    });

    // 设置错误回调
    channel->set_error_callback([link]() {
        LOG_ERROR(CLUSTER, "ClusterLink fd error: %s", link->node_name().c_str());
        link->disconnect();
    });

    // 监听读和写事件
    channel->enable_reading();
    channel->enable_writing();

    // 注册到 EventLoop
    event_loop_->update_channel(channel);

    // 保存 Channel 引用（用于后续注销）
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        link_channels_[fd] = channel;
    }

    LOG_INFO(CLUSTER, "Registered ClusterLink to EventLoop: fd=%d, node=%s",
             fd, link->node_name().c_str());
}

void ClusterConnection::unregister_link_from_loop(ClusterLink* link) {
    if (!event_loop_ || !link) {
        return;
    }

    // 必须用 registered_fd()：这条路径是在 ClusterLink::disconnect_and_notify 的断开
    // 回调里跑的，那时 fd_ 已经被置成 -1，拿 -1 去 find 永远不命中，Channel 就既不
    // 从 epoll 摘除也不 delete。
    int fd = link->registered_fd();
    Channel* channel = nullptr;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = link_channels_.find(fd);
        if (it != link_channels_.end()) {
            channel = it->second;
            link_channels_.erase(it);
        }
    }

    if (channel) {
        event_loop_->remove_channel(channel);
        delete channel;
        LOG_INFO(CLUSTER, "Unregistered ClusterLink from EventLoop: fd=%d", fd);
    }
}

namespace {

/// @brief sender_name 里那半截"我是谁"的形状
enum class BusNameShape {
    kLiteralAddress,  // 只可能是地址：数字 + 点/冒号（IPv4、IPv6）
    kHostname,        // 名字：localhost、node-1、db.internal
    kMalformed,       // 两者都不是：有空格、下划线、纯数字没有分隔符等
};

// 形状判定不校验合法性，只回答一个问题：这两个字符串在真实拓扑里可不可能天然相等。
// 名字与源地址本来就是两种东西（运维敲进 CLUSTER MEET 的是名字，入站 accept 看到的是
// IP），拿它们逐字比永远都不等。
//
// kMalformed 单独一档很重要：身份 key 必须精确匹配，否则 "127.0.0.1 "（尾部多一个空格）
// 这种写法就能绕过等值比较。
BusNameShape classify_bus_name(const std::string& s) {
    if (s.empty()) return BusNameShape::kMalformed;

    bool has_separator = false;
    bool has_non_hex_letter = false;
    for (const char c : s) {
        if (c >= '0' && c <= '9') continue;
        if (c == '.' || c == ':') {
            has_separator = true;
            continue;
        }
        const bool hex_letter = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (hex_letter) continue;
        if (c == '-' || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z')) {
            has_non_hex_letter = true;
            continue;
        }
        return BusNameShape::kMalformed;  // 空格、下划线、其它标点
    }

    if (has_non_hex_letter) return BusNameShape::kHostname;
    if (has_separator) return BusNameShape::kLiteralAddress;
    return BusNameShape::kMalformed;  // 纯数字（"42"）既不是地址也不是名字
}

}  // namespace

bool bus_sender_identity_accepted(const std::string& claimed_ip,
                                  const std::string& observed_ip,
                                  const bool sender_is_known_member,
                                  const BusMsgPlane plane) {
    // 空值一律不通过：观察不到来源不等于免检，声称里少了一半也不等于免检
    if (claimed_ip.empty() || observed_ip.empty()) {
        return false;
    }

    if (plane == BusMsgPlane::kData) {
        // 数据面（复制命令、REPLSYNC）不拿地址当凭据：hostname/NAT/Docker 之后
        // "对端自报是谁"与"这条链路在哪"本来就永远不等，按地址拦会把整条复制静默丢掉。
        // 拦的依据改成"这个名字已经在成员表里"——握手阶段不会发数据面报文，所以随机
        // 来客依然进不来。畸形形状同样不接受。
        return sender_is_known_member &&
               classify_bus_name(claimed_ip) != BusNameShape::kMalformed;
    }

    const BusNameShape claimed = classify_bus_name(claimed_ip);
    const BusNameShape observed = classify_bus_name(observed_ip);
    if (claimed == BusNameShape::kMalformed || observed == BusNameShape::kMalformed) {
        // 身份 key 必须精确匹配，不接受"形状都不对还互相放行"
        return false;
    }

    // 控制面：两侧都是字面地址时保持 #45 的防伪能力（冒充另一个成员直接拒）；任一侧
    // 是主机名时不做地址比对，否则用主机名敲的那条 CLUSTER MEET 永远通不过。
    if (claimed == BusNameShape::kLiteralAddress && observed == BusNameShape::kLiteralAddress) {
        return claimed_ip == observed_ip;
    }
    return true;
}

void ClusterConnection::handle_link_msg(ClusterMsg&& msg, ClusterLink* link) {
    // 从 sender_name 提取节点信息 (格式: ip:port)
    std::string sender_name_str(msg.header.sender_name, 40);
    // 找到第一个空字符的位置
    size_t null_pos = sender_name_str.find('\0');
    if (null_pos != std::string::npos) {
        sender_name_str = sender_name_str.substr(0, null_pos);
    }

    // 验证 sender_name 格式是否有效 (必须是 ip:port 格式)
    auto colon_pos = sender_name_str.find(':');
    if (colon_pos == std::string::npos || sender_name_str.empty()) {
        LOG_WARN(CLUSTER, "Invalid sender_name in message, ignoring");
        return;
    }

    // 发送者身份校验，理由见 cluster_connection.h 里那段注释：sender_name 是集群
    // 成员的身份 key，PFAIL 记名与 failover 法定人数都以它为单位计票，而这个端口
    // 不认证。控制面按地址形状对账，数据面按"是不是已认识的成员"。
    const std::string claimed_ip = sender_name_str.substr(0, colon_pos);
    const std::string observed_ip = link ? link->ip() : std::string();
    const bool is_data_plane =
        msg.header.type == static_cast<uint16_t>(ClusterMsgType::kRepData);
    const bool known_member =
        state_ != nullptr && state_->getNode(sender_name_str) != nullptr;
    if (!bus_sender_identity_accepted(claimed_ip, observed_ip, known_member,
                                      is_data_plane ? BusMsgPlane::kData : BusMsgPlane::kControl)) {
        // 计数进 CLUSTER INFO：以前这里只有一条 WARN，复制数据被静默丢掉的时候，
        // 从协议面上看不出任何东西——"可诊断"必须有凭据。日志按第 1 条与每 100 条
        // 采样，避免被攻击刷屏。
        const uint64_t total = state_ != nullptr ? state_->note_bus_identity_rejection() : 1;
        if (state_ == nullptr || total == 1 || total % 100 == 0) {
            LOG_WARN(CLUSTER,
                     "Bus packet claims %s but arrived on a link to %s (plane=%s, known_member=%d, "
                     "total rejections=%llu); dropping",
                     sender_name_str.c_str(),
                     observed_ip.empty() ? "<unknown>" : observed_ip.c_str(),
                     is_data_plane ? "data" : "control",
                     known_member ? 1 : 0,
                     static_cast<unsigned long long>(total));
        }
        return;
    }

    // 自称是本端自己的报文同样丢掉：那会让我们把伪造的数据写进自己的状态表，
    // 而 Redis 也是拒绝跟自己 MEET 的。
    if (sender_name_str == ClusterServer::instance().getMyNodeName()) {
        LOG_WARN(CLUSTER, "Bus packet claims our own node name %s, dropping",
                 sender_name_str.c_str());
        return;
    }

    // 根据消息类型处理
    if (msg.header.type == static_cast<uint16_t>(ClusterMsgType::kPing)) {
        // 收到 PING，回复 PONG
        link->send_pong();
        LOG_DEBUG(CLUSTER, "Received PING from %s, sent PONG", sender_name_str.c_str());
    } else if (msg.header.type == static_cast<uint16_t>(ClusterMsgType::kMeet)) {
        // 收到 MEET 消息，对端请求认识本端
        // 解析 ip:port
        std::string ip = sender_name_str.substr(0, colon_pos);
        std::string port_str = sender_name_str.substr(colon_pos + 1);
        if (ip.empty() || port_str.empty()) {
            LOG_WARN(CLUSTER, "Invalid MEET message sender format: %s", sender_name_str.c_str());
            return;
        }

        try {
            int port = std::stoi(port_str);
            // 调用 meet_callback_ 将对端添加到本端状态
            if (meet_callback_) {
                meet_callback_(ip, port);
            }

            // 回复 PONG 让对端知道本端收到了 MEET
            link->send_pong();
            LOG_INFO(CLUSTER, "Received MEET from %s, sent PONG", sender_name_str.c_str());
        } catch (...) {
            LOG_WARN(CLUSTER, "Invalid port in MEET message: %s", port_str.c_str());
        }
    } else if (msg.header.type == static_cast<uint16_t>(ClusterMsgType::kPong)) {
        // 收到 PONG 消息，对端确认了本端的 MEET
        // 调用 node_connected_callback_ 将对端添加到本端状态 (如果还没有)
        if (node_connected_callback_) {
            node_connected_callback_(sender_name_str);
        }
        LOG_DEBUG(CLUSTER, "Received PONG from %s", sender_name_str.c_str());
    } else if (msg.header.type == static_cast<uint16_t>(ClusterMsgType::kRepData)) {
        // 收到复制数据消息
        LOG_INFO(CLUSTER, "kRepData received from %s: %s",
                 sender_name_str.c_str(), msg.args.empty() ? "empty" : msg.args[0].c_str());
        if (!msg.args.empty()) {
            const std::string& cmd_line = msg.args[0];
            if (cmd_line.rfind("REPLSYNC:", 0) == 0) {
                // 这是复制同步请求: "REPLSYNC:<replica_name>"
                std::string replica_name = cmd_line.substr(9);
                LOG_INFO(CLUSTER, "Received replication sync request from %s (replica=%s)",
                         sender_name_str.c_str(), replica_name.c_str());
                auto& repl_mgr = ReplicationMgr::instance();
                // 获取副本节点信息
                auto node = state_->getNode(sender_name_str);
                if (node) {
                    auto& info = node->getInfo();
                    repl_mgr.add_replica(replica_name, info.ip, info.port, node);
                    repl_mgr.send_rdb_to_replica(replica_name);
                }
            } else {
                // 这是复制数据命令，在副本端执行。
                // 修复 P0-3：命令执行可能抛异常（畸形数据/类型错误等），
                // 无 catch 会沿 handle_read → EventLoop 一路上抛 → std::terminate
                try {
                    ReplicationMgr::instance().handle_replication_command(cmd_line);
                } catch (const std::exception& e) {
                    LOG_ERROR(CLUSTER, "Replication command '%s' threw: %s",
                              cmd_line.c_str(), e.what());
                } catch (...) {
                    LOG_ERROR(CLUSTER, "Replication command '%s' threw unknown exception",
                              cmd_line.c_str());
                }
            }
        }
    }

    // 先处理 gossip 回调（不需要移动 msg）
    if (gossip_callback_) {
        gossip_callback_(sender_name_str, msg);
    }

    // 再处理通用消息回调（移动 msg）
    if (msg_callback_) {
        msg_callback_(std::move(msg), link);
    }
}

void ClusterConnection::on_timer() {
    if (!heartbeat_running_) {
        return;
    }

    auto now = get_current_time_ms();

    // 检查是否需要发送心跳
    if (now - last_heartbeat_time_ms_ >= heartbeat_interval_ms_) {
        // 广播 PING 到所有连接的节点
        broadcast_ping();
        last_heartbeat_time_ms_ = now;
        LOG_DEBUG(CLUSTER, "Heartbeat: sent PING to all nodes");
    }

    // 检查节点超时
    check_connections();
}

void ClusterConnection::check_connections() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    // 先收集超时的节点名称，避免迭代过程中回调修改容器导致迭代器失效
    std::vector<std::string> timed_out_nodes;
    for (const auto& [name, link] : links_) {
        if (!link->is_connected()) {
            continue;
        }

        int64_t last_recv = link->last_recv_time();
        if (last_recv > 0 && (now - last_recv) > ping_timeout_ms_) {
            LOG_WARN(CLUSTER, "Node %s ping timeout (last_recv=%ld, now=%ld, timeout=%ld)",
                     name.c_str(), last_recv, now, ping_timeout_ms_);
            timed_out_nodes.push_back(name);
        }
    }

    // 释放锁后再触发回调，避免死锁和迭代器失效
    lock.unlock();

    for (const auto& name : timed_out_nodes) {
        // 标记节点为 PFAIL（疑似下线）：触发故障检测的入口。
        // 注意：这里只标 PFAIL；客观下线（FAIL）由 checkFailQuorum 在
        // 心跳线程中基于 PFAIL 报告数达到法定人数时升级，避免将瞬时抖动
        // 误判为永久下线。
        if (state_) {
            state_->markNodeAsPfail(name);
        }
        if (ping_timeout_callback_) {
            ping_timeout_callback_(name);
        }
    }
}

} // namespace cc_server