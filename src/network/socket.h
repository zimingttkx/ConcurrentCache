#ifndef CONCURRENTCACHE_NETWORK_SOCKET_H
#define CONCURRENTCACHE_NETWORK_SOCKET_H

#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <cstring>
#include <arpa/inet.h>

#include "../base/log.h"

namespace cc_server {
    /**
     * @brief Socket类：TCP 套接字封装
     *
     * 协作关系：
     * - 封装 Linux socket API，RAII 管理套接字生命周期
     * - bind_and_listen() 创建监听套接字
     * - accept() 接受客户端连接
     * - send()/recv() 数据收发
     * - 被 Connection 使用，管理客户端连接
     */
    class Socket {
    private:
        int fd_;

    public:
        Socket();
        explicit Socket(int fd);
        ~Socket();

        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        Socket(Socket&& other) noexcept;
        Socket& operator=(Socket&& other) noexcept;

        // 创建监听套接字
        bool bind_and_listen(int port, bool reuse = true);

        // 接受客户端连接
        int accept();

        // 数据收发
        ssize_t send(const void* buf, size_t len);
        ssize_t recv(void* buf, size_t len);

        // 关闭套接字
        void close();

        // 获取 fd
        int fd() const;
        bool valid() const;
    };
}

#endif CONCURRENTCACHE_NETWORK_SOCKET_H