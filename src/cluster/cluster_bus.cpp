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

        // 配额：总线端口不认证，任何对端都能一直连。超限就地关掉这条，
        // 而不是等 fd 耗尽把事件循环拖进空转。
        if (!inbound_admitted(link_count())) {
            const uint64_t refused = inbound_refused_.fetch_add(1) + 1;
            LOG_WARN(CLUSTER,
                     "ClusterBus: inbound link quota %zu reached, closing %s:%d (refused=%lu)",
                     kMaxInboundLinks, client_ip, client_port,
                     static_cast<unsigned long>(refused));
            ::close(client_fd);
            continue;
        }

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

    link->set_disconnect_callback([this](const std::string& name, ClusterLink* /*link*/) {
        // 修复 P0-2：此回调在 ClusterLink::disconnect_and_notify 内触发，
        // 回调返回后 link 即被销毁。remove_link 会销毁 link 并注销其 Channel，
        // 之后不能再向下游传递任何 link 指针（旧代码传 raw_link —— 悬空指针被
        // 下游 on_node_disconnected 解引用）。传 nullptr：bus 侧 Channel 已由
        // remove_link 负责注销，下游无需也无法再做。
        remove_link(name);
        if (disconnect_callback_) {
            disconnect_callback_(name, nullptr);
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

size_t ClusterBus::link_count() const {
    std::shared_lock<std::shared_mutex> lock(links_mutex_);
    return links_.size();
}

void ClusterBus::remove_link(const std::string& node_name) {
    // 注意：此函数由 ClusterLink::disconnect_and_notify 的断开回调触发，也就是说
    // 调用栈还在 Channel::handle_event() -> read_cb -> ClusterLink::handle_read() 里。
    // 所以这里既不能当场析构 link，也不能当场 delete 它的 Channel：
    //   - handle_event 拷出的 write_cb 捕获的是同一个 link 裸指针，而链路登记时就
    //     enable_writing()，EPOLLIN 与 EPOLLOUT 几乎总在同一条事件里回来 ——
    //     read_cb 返回后还会再调一次 write_cb，对象没了就是 heap-use-after-free；
    //   - delete Channel 更是直接释放正在执行的那个栈帧所属的对象。
    // 做法照 SubReactor::remove_connection 的既有模式：两者都交给 loop 线程，在下一
    // 轮分发之前的任务队列里回收。队列载体是 std::function，要求可拷贝，所以
    // unique_ptr 先转 shared_ptr 再捕获（C++20 还没有 move_only_function）。

    int fd_to_remove = -1;
    std::unique_ptr<ClusterLink> dying;

    {
        std::unique_lock<std::shared_mutex> lock(links_mutex_);
        auto it = links_.find(node_name);
        if (it != links_.end()) {
            // registered_fd() 而不是 fd()：本函数由断开回调触发，那时 fd_ 已经是 -1
            fd_to_remove = it->second->registered_fd();
            dying = std::move(it->second);
            links_.erase(it);
        }
    }

    // Channel 先从 epoll 与映射里摘掉（这一步必须立即做，否则本轮之后还可能再有事件
    // 打到这条已断的链路），对象本身连同 link 一起延后销毁。
    Channel* detached = (fd_to_remove >= 0) ? detach_link_channel(fd_to_remove) : nullptr;

    if (event_loop_ != nullptr && (detached != nullptr || dying != nullptr)) {
        std::shared_ptr<ClusterLink> keeper(std::move(dying));
        event_loop_->queue_in_loop([keeper, detached]() { delete detached; });
    } else {
        // 没有 loop 可用（理论上只在启动失败的路径上）：当场释放，不留泄漏
        delete detached;
    }

    if (fd_to_remove >= 0) {
        LOG_INFO(CLUSTER, "ClusterBus: removed link for %s (fd=%d)",
                 node_name.c_str(), fd_to_remove);
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

    // 走 link 自己的 handle_error()（内部是 disconnect_and_notify），不要写成
    // disconnect()：不通知的话 remove_link 不会被叫起，这条 handshake:* 条目、
    // 它的 Channel 与 link_channels_ 里的对象就永久留着。
    channel->set_error_callback([raw_link]() {
        raw_link->handle_error();
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

    int fd = link->registered_fd();  // 同上：断开回调里 fd_ 已是 -1
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

Channel* ClusterBus::detach_link_channel(int fd) {
    if (!event_loop_ || fd < 0) {
        return nullptr;
    }

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
        // 只从 epoll 与 channels_ 里摘掉；对象本身交给调用方处置（见 remove_link
        // 为什么要延后销毁）。
        event_loop_->remove_channel(channel);
    }
    return channel;
}

void ClusterBus::unregister_link_from_loop_fd(int fd) {
    delete detach_link_channel(fd);
}

} // namespace cc_server