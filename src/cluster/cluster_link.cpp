// cluster_link.cpp
#include "cluster_link.h"
#include "cluster_server.h"
#include "cluster_gossip.h"
#include "base/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <errno.h>

namespace cc_server {

ClusterLink::ClusterLink(const std::string& node_name, const std::string& ip, int port)
    : node_name_(node_name), ip_(ip), port_(port) {
    LOG_INFO(CLUSTER, "Created ClusterLink: node=%s, ip=%s, port=%d",
             node_name.c_str(), ip.c_str(), port);
}

ClusterLink::~ClusterLink() {
    disconnect();
}

bool ClusterLink::connect() {
    if (connected_.load()) {
        LOG_WARN(CLUSTER, "ClusterLink already connected: %s", node_name_.c_str());
        return true;
    }

    // 创建 socket
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERROR(CLUSTER, "Failed to create socket for %s: %s",
                 node_name_.c_str(), strerror(errno));
        return false;
    }
    registered_fd_ = fd_;

    // 设置非阻塞
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    // 连接对端
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR(CLUSTER, "Invalid IP address: %s", ip_.c_str());
        close(fd_);
        fd_ = -1;
        return false;
    }

    int ret = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        LOG_ERROR(CLUSTER, "Failed to connect to %s:%d: %s",
                 ip_.c_str(), port_, strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 非阻塞连接返回 EINPROGRESS，表示连接正在进行中
    // 连接真正建立完成需要等待可写事件，这里先标记为已连接
    // handle_write() 会在连接建立后处理后续操作
    connected_.store(true);
    update_last_recv_time();
    LOG_INFO(CLUSTER, "ClusterLink connected to %s (%s:%d)",
             node_name_.c_str(), ip_.c_str(), port_);
    return true;
}

void ClusterLink::disconnect() {
    // 保证只执行一次：先 CAS 把 connected_ 置 false，
    // 避免 handle_read/handle_write 与断开回调中对同一对象重复调用
    bool expected = true;
    if (!connected_.compare_exchange_strong(expected, false)) {
        return;
    }

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    LOG_INFO(CLUSTER, "ClusterLink disconnected: %s", node_name_.c_str());
}

void ClusterLink::disconnect_and_notify() {
    // 修复 P0-2（三重 UAF）：断开顺序必须是
    //   1) CAS 置 connected_=false —— 之后所有事件回调直接返回
    //   2) 关闭 fd
    //   3) 触发 disconnect_callback_（回调可能销毁 this）
    // 回调返回后，本函数（以及调用它的 handle_read/handle_write 事件路径）
    // 绝不再访问任何成员变量 —— this 可能已在回调中被 delete。
    // 旧代码先回调再 disconnect()，而 disconnect() 是成员函数，
    // CAS connected_ 就是在解引用已被销毁的对象。
    bool expected = true;
    if (!connected_.compare_exchange_strong(expected, false)) {
        return;  // 已断开/已通知过
    }

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    LOG_INFO(CLUSTER, "ClusterLink disconnected: %s", node_name_.c_str());

    if (disconnect_callback_) {
        disconnect_callback_(node_name_, this);
    }
    // 注意：此处 return 后不得再触碰 this 的任何成员。
}

namespace {

// 十进制位数（0→1、9→1、10→2）。长度字段的字节数由它决定，编码侧与
// 帧长计算侧必须用同一个算法，否则 header.length 会与真实字节数脱钩 ——
// 那是 P0-1 那一类"帧读完了"的错位。
size_t decimal_digits(uint64_t v) {
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

// 参数条数的上限。每条约少 2 字节（"0\n"），所以条数不可能超过参数区字节数；
// 这里再加一道显式额度，避免一个被写坏的 count 让解码循环跑几十亿次。
constexpr uint64_t kMaxArgsPerFrame = 1u << 20;

} // namespace

size_t bus_args_frame_bytes(const std::vector<std::string>& args) {
    size_t bytes = decimal_digits(args.size()) + 1;  // "<count>\n"
    for (const auto& arg : args) {
        bytes += decimal_digits(arg.size()) + 1 + arg.size();
    }
    return bytes;
}

void bus_args_encode(const std::vector<std::string>& args, std::string& out) {
    out += std::to_string(args.size());
    out.push_back('\n');
    for (const auto& arg : args) {
        out += std::to_string(arg.size());
        out.push_back('\n');
        out.append(arg.data(), arg.size());
    }
}

bool bus_args_decode(const char* data, size_t len, std::vector<std::string>& out,
                     std::string& err) {
    size_t pos = 0;

    auto read_field = [&](uint64_t& value, const char* what) -> bool {
        const size_t start = pos;
        while (pos < len && data[pos] != '\n') {
            ++pos;
        }
        if (pos >= len) {
            err = std::string("no newline terminating ") + what;
            return false;
        }
        const size_t width = pos - start;
        if (width == 0 || width > 10) {
            err = std::string("bad width of ") + what;
            return false;
        }
        uint64_t v = 0;
        for (size_t i = start; i < pos; ++i) {
            if (data[i] < '0' || data[i] > '9') {
                err = std::string("non-digit in ") + what;
                return false;
            }
            v = v * 10u + static_cast<uint64_t>(data[i] - '0');
        }
        ++pos;  // 吃掉 '\n'
        value = v;
        return true;
    };

    uint64_t count = 0;
    if (!read_field(count, "arg count")) {
        return false;
    }
    if (count > kMaxArgsPerFrame || count > len) {
        err = "implausible arg count";
        return false;
    }

    for (uint64_t i = 0; i < count; ++i) {
        uint64_t arg_len = 0;
        if (!read_field(arg_len, "arg length")) {
            return false;
        }
        if (arg_len > static_cast<uint64_t>(len - pos)) {
            err = "arg length exceeds remaining frame bytes";
            return false;
        }
        out.emplace_back(data + static_cast<long>(pos), static_cast<size_t>(arg_len));
        pos += static_cast<size_t>(arg_len);
    }

    // 参数区必须被正好消费完。剩下字节说明 count/长度与真实内容不符 ——
    // 与其把多余字节当成一个参数收进来，不如判畸形：错位的参数会被下游执行成
    // 另一条命令，那比断链严重得多。
    if (pos != len) {
        err = "trailing bytes after last arg";
        return false;
    }
    return true;
}

void bus_args_decode_legacy(const char* data, size_t len, std::vector<std::string>& out) {
    std::string current_arg;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\xC0') {
            out.push_back(current_arg);
            current_arg.clear();
        } else {
            current_arg.push_back(data[i]);
        }
    }
    if (!current_arg.empty()) {
        out.push_back(current_arg);
    }
}

bool ClusterLink::send_msg(const ClusterMsg& msg) {
    if (!connected_.load()) {
        LOG_WARN(CLUSTER, "Cannot send msg to disconnected link: %s", node_name_.c_str());
        return false;
    }

    // 构建消息头 + 内容
    ClusterMsgHeader header = msg.header;
    header.type = static_cast<uint16_t>(msg.header.type);

    // 如果 sender_name 未设置，从 ClusterServer 获取本节点名称
    if (header.sender_name[0] == '\0') {
        std::string my_name = cc_server::ClusterServer::instance().getMyNodeName();
        if (!my_name.empty()) {
            snprintf(header.sender_name, sizeof(header.sender_name), "%s", my_name.c_str());
        }
    }

    // 计算总长度（header + v2 参数帧）。
    // 用 size_t 累加后再校验，不能像原来那样直接 += 到 uint32 的 header.length 上：
    // 参数够多时它会回绕成一个小值，接收端于是提前判定"帧读完了"、后续字节流永久错位
    // ——那正是 P0-1 从 uint16 换成 uint32 时没有根治的那一半。
    // 上限与接收端 read_complete() 的 kMaxPacketBytes 对称：自己不能发出会被对端
    // 判为畸形并断链的帧。
    // 帧长与实际写入字节数共用 bus_args_* 这一套算法，两边不会各算各的。
    const size_t frame_bytes = sizeof(ClusterMsgHeader) + bus_args_frame_bytes(msg.args);
    if (frame_bytes > kMaxPacketBytes) {
        LOG_ERROR(CLUSTER, "Refusing to send frame of %zu bytes to %s: exceeds bus limit %u",
                  frame_bytes, node_name_.c_str(), kMaxPacketBytes);
        return false;
    }
    header.length = static_cast<uint32_t>(frame_bytes);
    // 参数区是按 v2 写的，所以版本也必须由这里定死：不能信调用方留在 msg 里的值，
    // 否则一个 msg.version = 1 会让接收端用 0xC0 规则去切 v2 帧，解出一堆错位参数。
    header.version = kBusFramingVersion;

    // 添加诊断日志（仅在非心跳消息时）
    if (header.type != 1 && header.type != 2) {
        LOG_INFO(CLUSTER, "SEND-MSG fd=%d node=%s type=%u args=%zu first_arg=%s",
                 fd_, node_name_.c_str(), header.type, msg.args.size(),
                 msg.args.empty() ? "-" : msg.args[0].c_str());
    }

    // 添加到发送缓冲区（加锁保护，不在此线程调用 handle_write，
    // 由 EventLoop 统一负责发送，避免多线程竞争 send_buffer_ 导致数据重复发送）
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_buffer_.append(reinterpret_cast<const char*>(&header), sizeof(header));

        std::string frame;
        frame.reserve(bus_args_frame_bytes(msg.args));
        bus_args_encode(msg.args, frame);
        send_buffer_.append(frame.data(), frame.size());
    }

    // 注意：不在此处调用 handle_write()。EventLoop 已通过 enable_writing()
    // 监听 EPOLLOUT 事件，会在 fd 可写时自动调用 handle_write() 发送数据。
    // 如果在调用线程直接发送，会与 EventLoop 线程产生竞态条件，
    // 导致同一份数据被 send() 两次，副本端收到重复命令。
    return true;
}

bool ClusterLink::send_ping() {
    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPing);
    return send_msg(msg);
}

bool ClusterLink::send_pong() {
    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPong);
    return send_msg(msg);
}

bool ClusterLink::send_meet(const std::string& my_ip, int my_port) {
    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kMeet);
    msg.args.push_back(my_ip);
    msg.args.push_back(std::to_string(my_port));
    return send_msg(msg);
}

bool ClusterLink::send_gossip(const GossipMsg& gossip_msg) {
    ClusterMsg msg;

    // 根据 GossipType 设置正确的 ClusterMsgType
    // 注意：GossipType 和 ClusterMsgType 的枚举值不同，需要映射
    // GossipType: kPing=1, kPong=2, kMeet=3, kFail=4, kFailoverAuthReq=5, kFailoverAuthAck=6, kPush=7, kPull=8
    // ClusterMsgType: kPing=1, kPong=2, kMeet=3, kMail=4, kFail=5, kPublish=6, kFailoverAuthReq=7, kFailoverAuthAck=8, kUpdate=9
    switch (gossip_msg.type) {
        case GossipType::kPing:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPing);
            break;
        case GossipType::kPong:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPong);
            break;
        case GossipType::kMeet:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kMeet);
            break;
        case GossipType::kFail:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kFail);
            break;
        case GossipType::kFailoverAuthReq:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kFailoverAuthReq);
            break;
        case GossipType::kFailoverAuthAck:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kFailoverAuthAck);
            break;
        case GossipType::kPush:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kUpdate);
            break;
        case GossipType::kPull:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kMail);
            break;
        default:
            msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPing);
            break;
    }

    msg.header.sender_epoch = gossip_msg.sender_epoch;
    strncpy(msg.header.sender_name, gossip_msg.sender_name.c_str(), sizeof(msg.header.sender_name) - 1);

    // 将Gossip消息的节点信息编码到args中
    // 格式: node_name,ip,port,flags,role[,failover_offset]
    for (const auto& node : gossip_msg.nodes) {
        std::string node_info = node.name + "," + node.ip + "," +
                                std::to_string(node.port) + "," +
                                std::to_string(node.flags) + "," +
                                std::to_string(static_cast<int>(node.role));

        // 如果有故障转移相关信息，添加到末尾
        if (node.failover_offset != 0) {
            node_info += "," + std::to_string(node.failover_offset);
        }

        msg.args.push_back(node_info);
    }

    LOG_DEBUG(CLUSTER, "Sending GOSSIP type=%d broadcast for %zu nodes",
              static_cast<int>(gossip_msg.type), gossip_msg.nodes.size());
    return send_msg(msg);
}

bool ClusterLink::send_raw(const std::string& data) {
    if (!connected_.load()) {
        LOG_WARN(CLUSTER, "Cannot send raw data to disconnected link: %s", node_name_.c_str());
        return false;
    }

    // 直接添加数据到发送缓冲区（由 EventLoop 统一发送）
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_buffer_.append(data.data(), data.size());
    }
    return true;
}

void ClusterLink::handle_read() {
    // disconnected_ 时立即返回：可能已被 disconnect_and_notify 断开，
    // 或者 this 正处于断开回调中（回调可能销毁 this）
    if (!connected_.load()) {
        return;
    }

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);

    if (n > 0) {
        recv_buffer_.append(buf, static_cast<size_t>(n));
        update_last_recv_time();

        // 处理接收到的数据
        while (!frame_invalid_) {
            if (!read_complete()) {
                break;
            }
            if (!decode_msg()) {
                // 协议错误（含非法 length 帧，见 read_complete）：
                // 必须断开，否则滞留的坏字节会让这条链路永久失效
                LOG_ERROR(CLUSTER, "Failed to decode message from %s, disconnecting",
                          node_name_.c_str());
                disconnect_and_notify();
                return;  // 回调可能已销毁 this，不得再访问成员
            }
        }

        // 畸形帧（length 装不下 header，或超过单帧上限）由 frame_invalid_ 单独表达，
        // 不能再"借道" read_complete() == true 去叫 decode_msg() 解析：那条路径会照
        // 声明长度去读参数区，而缓冲区里根本没有那么多字节。一个 2KB 的头写
        // length=0xFFFFFFFF，旧实现就会越过 recv_buffer_ 结尾读 4GB，进程直接段错误
        // ——而集群总线端口是不认证的输入面。现在这里统一断链。
        if (frame_invalid_) {
            LOG_ERROR(CLUSTER, "Malformed frame from %s, disconnecting", node_name_.c_str());
            disconnect_and_notify();
            return;  // 回调可能已销毁 this，不得再访问成员
        }

        // 滴包防护。不能用 last_recv_time_ 代替：每滴一个字节都会刷新它，于是
        // "帧头声明 256MB-1、之后每 10 秒送 1 字节" 永远不超时，而接收缓冲一路涨。
        // 这里量的是同一帧从第一次残缺起停留了多久，与流量无关。
        const uint64_t now = steady_now_ms();
        const size_t pending = recv_buffer_.readable_bytes();
        if (pending > kMaxPacketBytes) {
            // 兜底：正常路径下 read_complete() 一拿到头就按声明长度拒了，能堆到这里的
            // 是实打实收进来的字节。
            LOG_ERROR(CLUSTER, "Recv buffer from %s holds %zu bytes, disconnecting",
                      node_name_.c_str(), pending);
            disconnect_and_notify();
            return;  // 回调可能已销毁 this
        }

        bool frame_pending = false;
        uint32_t declared = 0;
        if (pending >= kHeaderSize) {
            uint32_t magic = 0;
            memcpy(&magic, recv_buffer_.peek(), sizeof(magic));
            if (magic == kMsgMagic) {
                memcpy(&declared, recv_buffer_.peek() + offsetof(ClusterMsgHeader, length),
                       sizeof(declared));
                frame_pending = static_cast<uint64_t>(declared) > pending;
            }
        } else if (pending > 0) {
            // 连帧头都还没读全，同样是一帧没着落。不记这一笔的话，对端只要每次滴
            // 不到 kHeaderSize 个字节，下面的记账就会每轮从零开始 —— 缓冲确实涨不动，
            // 但这条链路和它的 fd 可以无限期挂着。期限按头长折算，等于固定 10 秒宽限。
            declared = static_cast<uint32_t>(kHeaderSize);
            frame_pending = true;
        }

        if (partial_frame_expired(frame_pending ? declared : 0, pending, now)) {
            LOG_ERROR(CLUSTER,
                      "Frame from %s stuck at %zu/%u bytes past its deadline, disconnecting",
                      node_name_.c_str(), pending, declared);
            disconnect_and_notify();
            return;  // 回调可能已销毁 this
        }
    } else if (n == 0) {
        // 对端关闭连接
        LOG_INFO(CLUSTER, "Connection closed by %s", node_name_.c_str());
        // disconnect_and_notify 内部先断链再触发回调；
        // 回调（可能销毁 this）返回后立即 return，不再访问任何成员。
        // （修复 P0-2：旧代码先触发回调销毁 this，再调用成员函数 disconnect() —— UAF）
        disconnect_and_notify();
        return;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR(CLUSTER, "Read error from %s: %s", node_name_.c_str(), strerror(errno));
        disconnect_and_notify();
        return;
    }
}

void ClusterLink::handle_write() {
    if (!connected_.load()) {
        return;
    }

    // 只在锁内操作 send_buffer_。断开处理不能在持锁时同步触发回调
    // （回调可能销毁 this → 析构函数再拿 send_mutex_ → 死锁/递归），
    // 先记录需要断开，解锁后再走 disconnect_and_notify。
    bool need_disconnect = false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);

        while (send_buffer_.readable_bytes() != 0) {
            ssize_t n = send(fd_, send_buffer_.peek(), send_buffer_.readable_bytes(), 0);

            if (n > 0) {
                send_buffer_.retrieve(static_cast<size_t>(n));
            } else if (n == 0) {
                LOG_INFO(CLUSTER, "Connection closed by %s during write", node_name_.c_str());
                need_disconnect = true;
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR(CLUSTER, "Write error to %s: %s", node_name_.c_str(), strerror(errno));
                need_disconnect = true;
                break;
            } else {
                // EAGAIN/EWOULDBLOCK - 发送缓冲区满，稍后再试
                break;
            }
        }
    }

    if (need_disconnect) {
        // 解锁后断开：回调可能销毁 this，返回后立即 return
        disconnect_and_notify();
        return;
    }
}

bool ClusterLink::read_complete() {
    const char* data = recv_buffer_.peek();
    size_t len = recv_buffer_.readable_bytes();

    // 至少要能读 header
    if (len < kHeaderSize) {
        return false;
    }

    // 检查 magic
    uint32_t magic;
    memcpy(&magic, data, sizeof(magic));
    if (magic != kMsgMagic) {
        // RESP 协议数据（如 CLUSTER MEET 的握手数据）可能到达 cluster bus 端口
        // 静默跳过非 cluster 协议数据，避免错误日志泛滥
        size_t discard = 0;
        for (size_t i = 0; i < len; i++) {
            if (static_cast<uint8_t>(data[i]) == 0x43 && i + kHeaderSize <= len) {
                uint32_t probe;
                memcpy(&probe, data + i, sizeof(probe));
                if (probe == kMsgMagic) {
                    break; // 找到下一个有效的 cluster 消息
                }
            }
            discard++;
        }
        if (discard > 0) {
            LOG_DEBUG(CLUSTER, "Skipping %zu non-cluster bytes from %s", discard, node_name_.c_str());
            recv_buffer_.retrieve(discard);
        }
        return false;
    }

    // 获取消息长度（uint32，含 header）
    uint32_t msg_len;
    memcpy(&msg_len, data + offsetof(ClusterMsgHeader, length), sizeof(msg_len));

    // 修复 P0-1（远程 DoS 死循环）：length 必须至少能容纳 header。
    // 旧代码 length 可为 0..kHeaderSize-1：read_complete 返回 true 而 decode_msg
    // 消费 0 字节（retrieve(0)），handle_read 的 while(read_complete()) 永不退出
    // → EventLoop 线程 100% CPU 死循环，整台服务器失去响应。
    //
    // 这里用 frame_invalid_ 表达"畸形"，而不是继续返回 true 让 decode_msg 去失败：
    // decode_msg 一旦被叫起来，就会按 header.length 读参数区，而缓冲区里没有那些字节。
    if (msg_len < kHeaderSize) {
        LOG_ERROR(CLUSTER, "Malformed frame from %s: length=%u < header_size=%zu",
                  node_name_.c_str(), msg_len, kHeaderSize);
        frame_invalid_ = true;
        return false;
    }

    // 单帧上限（kMaxPacketBytes，值本身见 cluster_link.h 里的注释：它被大 value 的
    // 复制帧绑住了，不能贸然按 Redis 的 2MB 收）。判断时机是对的——拿到 header 就能判，
    // 不必等正文到齐，因此这里不需要"积累到某个大小再拒"。
    // 关键是拒绝方式：必须置 frame_invalid_ 并返回 false，绝不能返回 true 让
    // decode_msg 起来——那正是越过缓冲区结尾读几十 GB 的入口。
    if (msg_len > kMaxPacketBytes) {
        LOG_ERROR(CLUSTER, "Oversized frame from %s: length=%u > limit=%u",
                  node_name_.c_str(), msg_len, kMaxPacketBytes);
        frame_invalid_ = true;
        return false;
    }

    return len >= msg_len;
}

bool ClusterLink::decode_msg() {
    const char* data = recv_buffer_.peek();

    if (!read_complete()) {
        return false;
    }

    ClusterMsg msg;
    memcpy(&msg.header, data, kHeaderSize);

    // 防御：read_complete 对非法帧返回 true（走断链），这里必须同步校验，
    // 避免 length < kHeaderSize 时 size_t 下溢
    if (msg.header.length < kHeaderSize) {
        return false;
    }

    // 解析参数区。v2 是"条数 + 每条 <字节数>\n<原始字节>"；v1 是裸 \xC0 切分，
    // 只作为升级窗口内旧对端的兼容路径保留（旧节点的 gossip 参数是 ASCII，
    // 不含 0xC0，所以控制面照常；数据面本来就会被 0xC0 切断，那正是这次改的东西）。
    // 其它版本号一律当畸形——按任何一套规则去解都可能解出错位参数并被下游执行。
    const size_t args_size = msg.header.length - kHeaderSize;
    const char* args_start = data + kHeaderSize;
    if (msg.header.version == kBusFramingVersion) {
        std::string err;
        if (!bus_args_decode(args_start, args_size, msg.args, err)) {
            LOG_ERROR(CLUSTER, "Malformed v2 args frame from %s (%zu bytes): %s",
                      node_name_.c_str(), args_size, err.c_str());
            return false;
        }
    } else if (msg.header.version == kBusFramingLegacyVersion) {
        if (args_size > 0) {
            bus_args_decode_legacy(args_start, args_size, msg.args);
        }
    } else {
        LOG_ERROR(CLUSTER, "Unsupported cluster bus framing version %u from %s",
                  static_cast<unsigned>(msg.header.version), node_name_.c_str());
        return false;
    }

    // 跳过已处理的数据
    recv_buffer_.retrieve(msg.header.length);

    // 诊断：记录每个解码消息的来源 fd 和链接名
    LOG_INFO(CLUSTER, "DECODE-MSG fd=%d node=%s type=%u args=%zu",
             fd_, node_name_.c_str(), msg.header.type, msg.args.size());

    // 调用消息回调
    if (msg_callback_) {
        msg_callback_(std::move(msg), this);
    }

    return true;
}

void ClusterLink::update_last_recv_time() {
    last_recv_time_.store(static_cast<int64_t>(steady_now_ms()));
}

uint64_t ClusterLink::steady_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}

bool ClusterLink::partial_frame_expired(const uint32_t declared_len, const size_t buffered,
                                       const uint64_t now_ms) {
    if (declared_len == 0) {
        // 没有半帧挂着：要么还没读到头，要么这一帧已经凑齐交给 read_complete。
        reset_partial_frame_accounting();
        return false;
    }

    // 只在"换了一帧"时重新起算：声明长度变了，或者缓冲变短了（上一条被消费掉或
    // 被 read_complete 丢弃）。注意滴包不会在这里重置——它每滴一个字节都在长，
    // 如果按进展重新起算，"每 10 秒滴 1 字节"就永远超不了时，防护等于没有。
    if (partial_frame_since_ms_ == 0 || declared_len != partial_frame_declared_ ||
        buffered < partial_frame_bytes_) {
        partial_frame_since_ms_ = now_ms;
        partial_frame_bytes_ = buffered;
        partial_frame_declared_ = declared_len;
        return false;
    }

    partial_frame_bytes_ = buffered;

    // 期限 = 固定宽限 + 按声明长度折算的时间（下限 1MB/s，见头文件注释）。
    // 真实链路远快于 1MB/s，所以大 value 复制帧不会被误杀；而滴包要凑完 256MB
    // 得几十年，一定超。
    const uint64_t limit_ms =
        kPartialFrameGraceMs + (declared_len / kPartialFrameMinBytesPerSec) * 1000ull;
    return now_ms - partial_frame_since_ms_ >= limit_ms;
}

void ClusterLink::reset_partial_frame_accounting() {
    partial_frame_since_ms_ = 0;
    partial_frame_bytes_ = 0;
    partial_frame_declared_ = 0;
}

void ClusterLink::set_partial_frame_start_for_test(const uint64_t started_ms) {
    // 测试接缝：把"这一帧第一次残缺"的时刻往前挪，好在没有真实 200 多秒的情况下
    // 驱动 handle_read 里的超时断链分支。与 set_fd 属于同一类只用于观测的接缝，
    // 不参与任何业务路径的决策。
    partial_frame_since_ms_ = started_ms;
}

} // namespace cc_server