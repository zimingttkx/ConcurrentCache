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
    if (!connected_.load()) {
        return;
    }

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    connected_.store(false);
    LOG_INFO(CLUSTER, "ClusterLink disconnected: %s", node_name_.c_str());
}

bool ClusterLink::send_msg(const ClusterMsg& msg) {
    if (!connected_.load()) {
        LOG_WARN(CLUSTER, "Cannot send msg to disconnected link: %s", node_name_.c_str());
        return false;
    }

    // 构建消息头 + 内容
    ClusterMsgHeader header = msg.header;
    header.type = static_cast<uint16_t>(msg.header.type);
    header.length = static_cast<uint16_t>(sizeof(header));

    // 如果 sender_name 未设置，从 ClusterServer 获取本节点名称
    if (header.sender_name[0] == '\0') {
        std::string my_name = cc_server::ClusterServer::instance().getMyNodeName();
        if (!my_name.empty()) {
            snprintf(header.sender_name, sizeof(header.sender_name), "%s", my_name.c_str());
        }
    }

    // 计算参数总长度
    for (const auto& arg : msg.args) {
        header.length += static_cast<uint16_t>(arg.size() + 1);  // +1 for separator
    }

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

        for (const auto& arg : msg.args) {
            send_buffer_.append(arg.data(), arg.size());
            send_buffer_.append("\xC0", 1);  // 参数分隔符
        }
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
    if (!connected_.load()) {
        return;
    }

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);

    if (n > 0) {
        recv_buffer_.append(buf, static_cast<size_t>(n));
        update_last_recv_time();

        // 处理接收到的数据
        while (read_complete()) {
            if (!decode_msg()) {
                LOG_ERROR(CLUSTER, "Failed to decode message from %s", node_name_.c_str());
                break;
            }
        }
    } else if (n == 0) {
        // 对端关闭连接
        LOG_INFO(CLUSTER, "Connection closed by %s", node_name_.c_str());
        if (disconnect_callback_) {
            disconnect_callback_(node_name_, this);
        }
        disconnect();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR(CLUSTER, "Read error from %s: %s", node_name_.c_str(), strerror(errno));
        if (disconnect_callback_) {
            disconnect_callback_(node_name_, this);
        }
        disconnect();
    }
}

void ClusterLink::handle_write() {
    if (!connected_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(send_mutex_);

    while (send_buffer_.readable_bytes() != 0) {
        ssize_t n = send(fd_, send_buffer_.peek(), send_buffer_.readable_bytes(), 0);

        if (n > 0) {
            send_buffer_.retrieve(static_cast<size_t>(n));
        } else if (n == 0) {
            // 连接被关闭
            LOG_INFO(CLUSTER, "Connection closed by %s during write", node_name_.c_str());
            if (disconnect_callback_) {
                disconnect_callback_(node_name_, this);
            }
            disconnect();
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR(CLUSTER, "Write error to %s: %s", node_name_.c_str(), strerror(errno));
            if (disconnect_callback_) {
                disconnect_callback_(node_name_, this);
            }
            disconnect();
            break;
        } else {
            // EAGAIN/EWOULDBLOCK - 发送缓冲区满，稍后再试
            break;
        }
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

    // 获取消息长度
    uint16_t msg_len;
    memcpy(&msg_len, data + offsetof(ClusterMsgHeader, length), sizeof(msg_len));

    return len >= msg_len;
}

bool ClusterLink::decode_msg() {
    const char* data = recv_buffer_.peek();

    if (!read_complete()) {
        return false;
    }

    ClusterMsg msg;
    memcpy(&msg.header, data, kHeaderSize);

    // 解析参数：header 之后的数据以 \xC0 分隔
    size_t args_size = msg.header.length - kHeaderSize;
    if (args_size > 0) {
        const char* args_start = data + kHeaderSize;
        const char* args_end = args_start + args_size;

        std::string current_arg;
        for (const char* p = args_start; p < args_end; ++p) {
            if (*p == '\xC0') {
                msg.args.push_back(current_arg);
                current_arg.clear();
            } else {
                current_arg += *p;
            }
        }
        // 如果末尾没有分隔符，添加最后一个参数
        if (!current_arg.empty()) {
            msg.args.push_back(current_arg);
        }
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
    last_recv_time_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace cc_server