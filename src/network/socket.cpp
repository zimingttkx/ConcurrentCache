#include "socket.h"

namespace cc_server {
    // 默认构造：fd_ 初始化为 -1（无效状态）
    Socket::Socket() : fd_(-1) {}

    // 用已有 fd 构造
    Socket::Socket(int fd) : fd_(fd) {}

    // 析构：调用 close() 关闭套接字
    Socket::~Socket() {
        close();
    }

    // 移动构造：接管对方 fd，原对象 fd_ 置 -1
    Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    // 移动赋值：先关闭自身，再接管对方 fd
    Socket& Socket::operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // 创建 TCP 监听套接字
    // - socket() 创建非阻塞套接字
    // - setsockopt() 开启地址复用
    // - bind() 绑定端口
    // - listen() 开始监听
    bool Socket::bind_and_listen(int port, bool reuse) {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd_ < 0) {
            LOG_ERROR(socket, "Socket() create failed: %s", strerror(errno));
            return false;
        }

        if (reuse) {
            int opt = 1;
            setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(fd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            LOG_ERROR(socket, "Bind() port %d failed: %s", port, strerror(errno));
            close();
            return false;
        }

        if (listen(fd_, 1024) < 0) {
            LOG_ERROR(socket, "Listen() port %d failed: %s", port, strerror(errno));
            close();
            return false;
        }
        LOG_INFO(socket, "Server started on port %d", port);
        return true;
    }

    // 接受客户端连接
    // - accept4() 非阻塞接受
    // - 打印客户端 IP 日志
    int Socket::accept() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = ::accept4(
            fd_,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len,
            SOCK_NONBLOCK
        );
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR(socket, "Accept() failed: %s", strerror(errno));
            }
            return -1;
        }
        LOG_INFO(socket, "Client connected: fd=%d, ip=%s", client_fd, inet_ntoa(client_addr.sin_addr));
        return client_fd;
    }

    // 发送数据
    ssize_t Socket::send(const void* buf, size_t len) {
        if (fd_ < 0) return -1;
        return ::send(fd_, buf, len, 0);
    }

    // 接收数据
    ssize_t Socket::recv(void* buf, size_t len) {
        if (fd_ < 0) return -1;
        return ::recv(fd_, buf, len, 0);
    }

    // 关闭套接字
    void Socket::close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int Socket::fd() const {
        return fd_;
    }

    bool Socket::valid() const {
        return fd_ >= 0;
    }
}