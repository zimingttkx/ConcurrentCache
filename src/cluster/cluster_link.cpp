// cluster_link.cpp
#include "cluster_link.h"
#include "cluster_server.h"
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
    addr.sin_port = htons(port_);

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
        header.length += arg.size() + 1;  // +1 for separator
    }

    // 添加到发送缓冲区
    send_buffer_.append(reinterpret_cast<const char*>(&header), sizeof(header));

    for (const auto& arg : msg.args) {
        send_buffer_.append(arg.data(), arg.size());
        send_buffer_.append("\xC0", 1);  // 参数分隔符
    }

    handle_write();
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

void ClusterLink::handle_read() {
    if (!connected_.load()) {
        return;
    }

    char buf[4096];
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);

    if (n > 0) {
        recv_buffer_.append(buf, n);
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

    while (send_buffer_.readable_bytes() != 0) {
        ssize_t n = send(fd_, send_buffer_.peek(), send_buffer_.readable_bytes(), 0);

        if (n > 0) {
            send_buffer_.retrieve(n);
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
        LOG_ERROR(CLUSTER, "Invalid message magic from %s: 0x%x", node_name_.c_str(), magic);
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

    // 跳过已处理的数据
    recv_buffer_.retrieve(msg.header.length);

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