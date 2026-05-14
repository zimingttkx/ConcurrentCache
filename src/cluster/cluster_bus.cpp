// cluster_bus.cpp
#include "cluster_bus.h"
#include "cluster_server.h"
#include "base/log.h"
#include "../network/socket.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace cc_server {

ClusterBus::ClusterBus()
    : listen_fd_(-1), listen_channel_(nullptr) {
    LOG_INFO(CLUSTER, "ClusterBus created");
}

ClusterBus::~ClusterBus() {
    stop();
    LOG_INFO(CLUSTER, "ClusterBus destroyed");
}

void ClusterBus::init(EventLoop* loop) {
    event_loop_ = loop;
    LOG_INFO(CLUSTER, "ClusterBus initialized");
}

bool ClusterBus::start(int server_port) {
    if (running_.load()) {
        LOG_WARN(CLUSTER, "ClusterBus already running");
        return true;
    }

    if (!event_loop_) {
        LOG_ERROR(CLUSTER, "ClusterBus: EventLoop not set");
        return false;
    }

    // 计算集群总线端口（server_port + 10000）
    int bus_port = server_port + 10000;

    // 创建监听 socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR(CLUSTER, "ClusterBus: failed to create socket: %s", strerror(errno));
        return false;
    }

    // 设置 SO_REUSEADDR 和 SO_REUSEPORT
    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN(CLUSTER, "ClusterBus: setsockopt SO_REUSEADDR failed: %s", strerror(errno));
    }
#ifdef SO_REUSEPORT
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        LOG_WARN(CLUSTER, "ClusterBus: setsockopt SO_REUSEPORT failed: %s", strerror(errno));
    }
#endif

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(bus_port));

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR(CLUSTER, "ClusterBus: failed to bind to port %d: %s", bus_port, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // 监听（使用较大 backlog）
    int backlog = SOMAXCONN > 4096 ? SOMAXCONN : 4096;
    if (listen(listen_fd_, backlog) < 0) {
        LOG_ERROR(CLUSTER, "ClusterBus: failed to listen on port %d: %s", bus_port, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // 设置为非阻塞
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERROR(CLUSTER, "ClusterBus: fcntl F_GETFL failed: %s", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR(CLUSTER, "ClusterBus: fcntl F_SETFL failed: %s", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // 创建 Channel 并注册到 EventLoop
    listen_channel_ = new Channel(event_loop_, listen_fd_);

    // 设置回调
    listen_channel_->set_read_callback([this]() {
        handle_accept();
    });

    listen_channel_->set_error_callback([this]() {
        LOG_ERROR(CLUSTER, "ClusterBus: listen socket error");
        stop();
    });

    // 监听读事件（接受新连接）
    listen_channel_->enable_reading();
    event_loop_->update_channel(listen_channel_);

    listen_port_ = bus_port;
    running_.store(true);

    LOG_INFO(CLUSTER, "ClusterBus started, listening on port %d", bus_port);
    return true;
}

void ClusterBus::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // 关闭监听 socket，防止新的 accept
    if (listen_channel_) {
        event_loop_->remove_channel(listen_channel_);
        delete listen_channel_;
        listen_channel_ = nullptr;
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // 清除回调，防止在销毁 link 时被调用
    // 此时不再有新的 accept 发生，所以 handle_accept 不会再创建新链接
    disconnect_callback_ = nullptr;
    msg_callback_ = nullptr;

    // 先清理所有 link channels（从 EventLoop 注销并删除）
    // 这样确保即使有新的 link 被创建（race condition），其 channel 也不会被清理
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        for (auto& [fd, channel] : link_channels_) {
            if (event_loop_) {
                event_loop_->remove_channel(channel);
            }
            delete channel;
        }
        link_channels_.clear();
    }

    // 最后清空 links_（删除所有 Link）
    // 此时所有 channels 已清理，不会出现 Channel 被删除但 Link 还在的情况
    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        links_.clear();
    }

    LOG_INFO(CLUSTER, "ClusterBus stopped");
}

void ClusterBus::handle_accept() {
    if (!running_.load()) {
        return;
    }

    // 接受所有等待的连接
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多连接
                break;
            }
            LOG_ERROR(CLUSTER, "ClusterBus: accept failed: %s", strerror(errno));
            break;
        }

        // 获取客户端地址
        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == nullptr) {
            LOG_ERROR(CLUSTER, "ClusterBus: inet_ntop failed");
            ::close(client_fd);
            continue;
        }
        int client_port = ntohs(client_addr.sin_port);

        // 生成临时节点名称（格式: handshake:ip:port）
        // 注意：对端的真实节点名称由其 gossip 消息中的 sender_name 字段确定
        std::string node_name = "handshake:" + std::string(client_ip) + ":" + std::to_string(client_port);

        // 设置为非阻塞
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags < 0) {
            LOG_ERROR(CLUSTER, "ClusterBus: fcntl F_GETFL failed: %s", strerror(errno));
            ::close(client_fd);
            continue;
        }
        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            LOG_ERROR(CLUSTER, "ClusterBus: fcntl F_SETFL failed: %s", strerror(errno));
            ::close(client_fd);
            continue;
        }

        // 创建 ClusterLink
        ClusterLink* link = create_link(client_fd, node_name, client_ip, client_port);

        LOG_INFO(CLUSTER, "ClusterBus: accepted connection from %s (fd=%d)", node_name.c_str(), client_fd);

        // 立即尝试读取可能已在缓冲区中的数据（对端可能在 connect 后立即发送了消息）
        // 这避免了等待下一个 epoll 周期，消除了 MEET/PING 处理的竞态条件
        link->handle_read();
    }
}

ClusterLink* ClusterBus::create_link(int fd, const std::string& node_name, const std::string& ip, int port) {
    auto link = std::make_unique<ClusterLink>(node_name, ip, port);

    // 保存 link 的原始指针用于回调
    ClusterLink* raw_link = link.get();

    link->set_msg_callback([this](ClusterMsg&& msg, ClusterLink* cluster_link) {
        if (msg_callback_) {
            msg_callback_(std::move(msg), cluster_link);
        }
    });

    link->set_disconnect_callback([this, raw_link](const std::string& name, ClusterLink* /*link*/) {
        // 注意：这里的 link 参数可能是悬空指针，使用传入的 raw_link 代替
        remove_link(name);
        if (disconnect_callback_) {
            disconnect_callback_(name, raw_link);
        }
    });

    // 直接设置 fd（不调用 connect，因为是入站连接）
    link->set_fd(fd);

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        links_[node_name] = std::move(link);
    }

    // 注册到 EventLoop
    if (event_loop_) {
        register_link_to_loop(raw_link);
    }

    return raw_link;
}

void ClusterBus::remove_link(const std::string& node_name) {
    // 注意：此函数可能在 ClusterLink 的析构函数中调用
    // 必须先从 EventLoop 注销 Channel，然后再销毁 ClusterLink
    // 否则会出现 Use-After-Free

    int fd_to_remove = -1;

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        auto it = links_.find(node_name);
        if (it != links_.end()) {
            // 先保存 fd，但不要获取 link 指针（因为马上要销毁）
            fd_to_remove = it->second->fd();
            links_.erase(it);
            // ClusterLink 在这里被销毁
        }
    }

    // 在 ClusterLink 销毁后，再注销 EventLoop 中的 Channel
    if (fd_to_remove >= 0) {
        if (event_loop_) {
            unregister_link_from_loop_fd(fd_to_remove);
        }
        LOG_INFO(CLUSTER, "ClusterBus: removed link for %s", node_name.c_str());
    }
}

void ClusterBus::register_link_to_loop(ClusterLink* link) {
    if (!event_loop_ || !link) {
        return;
    }

    int fd = link->fd();
    if (fd < 0) {
        return;
    }

    // 创建 Channel
    auto* channel = new Channel(event_loop_, fd);

    // 保存 link 的原始指针用于回调
    // 注意：由于 stop() 会先清空 links_ 再删除 Channel，
    // 因此在 Channel 活跃期间，link 一定有效
    ClusterLink* raw_link = link;

    // 设置回调
    channel->set_read_callback([raw_link]() {
        raw_link->handle_read();
    });

    channel->set_write_callback([raw_link]() {
        raw_link->handle_write();
    });

    channel->set_error_callback([raw_link]() {
        LOG_ERROR(CLUSTER, "ClusterBus: link fd error: %s", raw_link->node_name().c_str());
        raw_link->disconnect();
    });

    // 监听读和写事件
    channel->enable_reading();
    channel->enable_writing();

    // 注册到 EventLoop
    event_loop_->update_channel(channel);

    // 保存 Channel 引用以便后续清理
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        link_channels_[fd] = channel;
    }

    LOG_INFO(CLUSTER, "ClusterBus: registered link to EventLoop: fd=%d, node=%s",
             fd, link->node_name().c_str());
}

void ClusterBus::unregister_link_from_loop(ClusterLink* link) {
    if (!event_loop_ || !link) {
        return;
    }

    int fd = link->fd();
    Channel* channel = nullptr;

    // Find and remove channel from our map
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = link_channels_.find(fd);
        if (it != link_channels_.end()) {
            channel = it->second;
            link_channels_.erase(it);
        }
    }

    // Remove from EventLoop and delete
    if (channel) {
        event_loop_->remove_channel(channel);
        delete channel;
        LOG_DEBUG(CLUSTER, "ClusterBus: unregistered link from loop: fd=%d", fd);
    }
}

void ClusterBus::unregister_link_from_loop_fd(int fd) {
    if (!event_loop_ || fd < 0) {
        return;
    }

    Channel* channel = nullptr;

    // Find and remove channel from our map
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = link_channels_.find(fd);
        if (it != link_channels_.end()) {
            channel = it->second;
            link_channels_.erase(it);
        }
    }

    // Remove from EventLoop and delete
    if (channel) {
        event_loop_->remove_channel(channel);
        delete channel;
        LOG_DEBUG(CLUSTER, "ClusterBus: unregistered link from loop (fd): fd=%d", fd);
    }
}

} // namespace cc_server