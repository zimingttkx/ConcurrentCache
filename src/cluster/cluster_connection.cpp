// cluster_connection.cpp
#include "cluster_connection.h"
#include "cluster_server.h"
#include "cluster_gossip.h"
#include "base/log.h"
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
    last_heartbeat_time_ms_ = get_current_time_ms();
    LOG_INFO(CLUSTER, "Heartbeat started (interval=%ldms, timeout=%ldms)",
             heartbeat_interval_ms_, ping_timeout_ms_);
}

void ClusterConnection::stop_heartbeat() {
    if (!heartbeat_running_) {
        return;
    }
    heartbeat_running_ = false;
    LOG_INFO(CLUSTER, "Heartbeat stopped");
}

bool ClusterConnection::connect_to_node(const std::string& node_name,
                                       const std::string& ip, int port) {
    // 先检查是否已存在连接
    {
        std::shared_lock<std::shared_mutex> lock(links_mutex_);
        if (links_.find(node_name) != links_.end()) {
            LOG_INFO(CLUSTER, "Already connected to node: %s", node_name.c_str());
            return true;
        }
    }

    // 创建新连接
    auto link = std::make_unique<ClusterLink>(node_name, ip, port);

    // 设置消息回调 - 处理收到的消息
    link->set_msg_callback([this](ClusterMsg&& msg, ClusterLink* link) {
        handle_link_msg(std::move(msg), link);
    });

    // 设置断开回调
    link->set_disconnect_callback([this](const std::string& name, ClusterLink* link) {
        on_node_disconnected(name, link);
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
        links_[node_name] = std::move(link);
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
    ClusterLink* link_ptr = nullptr;

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);

        auto it = links_.find(node_name);
        if (it != links_.end()) {
            link_ptr = it->second.get();
            links_.erase(it);
        }
    }

    if (link_ptr) {
        // 先从 EventLoop 注销 Channel
        if (event_loop_) {
            unregister_link_from_loop(link_ptr);
        }
        link_ptr->disconnect();
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

void ClusterConnection::broadcast_ping() {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    for (auto& [name, link] : links_) {
        if (link->is_connected()) {
            link->send_ping();
        }
    }
}

void ClusterConnection::broadcast_pong() {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    for (auto& [name, link] : links_) {
        if (link->is_connected()) {
            link->send_pong();
        }
    }
}

void ClusterConnection::broadcast_gossip(const GossipMsg& msg) {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);

    for (auto& [name, link] : links_) {
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
    // 先从 EventLoop 注销 Channel
    if (link && event_loop_) {
        unregister_link_from_loop(link);
    }

    std::unique_lock<std::shared_mutex> lock(links_mutex_);
    links_.erase(node_name);

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

    int fd = link->fd();

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
        if (ping_timeout_callback_) {
            ping_timeout_callback_(name);
        }
    }
}

} // namespace cc_server