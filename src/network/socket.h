#ifndef CONCURRENTCACHE_NETWORK_SOCKET_H
#define CONCURRENTCACHE_NETWORK_SOCKET_H

#include <string>
#include <sys/socket.h> // socket() bind() listen() accept()系统调用
#include <netinet/in.h> // 网络地址结构体
#include <unistd.h> // close()系统调用
#include <errno.h>
#include <fcntl.h>
#include <cstring>
#include <arpa/inet.h> // inet_ntop() inet_pton()系统调用

#include "../base/log.h"

namespace cc_server {
    // Socket类 封装一个TCP套接字的生命周期
    class Socket {
    private:
        int fd_; // 文件描述符 代表一个TCP套接字
    public:
        // 构造函数 创建空的Socket(fd = -1 无效状态)
        Socket() : fd_(-1){}

        // 显式构造 使用已有的fd创建socket
        explicit Socket(int fd) : fd_(fd) {};

        // 析构函数
        ~Socket(){close();}

        // 一个文件描述符不能对多个Socket对象管理 否则会被重复关闭
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        // 移动构造: 将一个Socket对象的所有权转移到新的Socket对象
        Socket(Socket&& other) noexcept : fd_(other.fd_) {
            // 把源对象的fd设置为-1避免重复关闭
            other.fd_ = -1;
        }

        // 移动赋值: 将一个Socket对象的所有权转移到新的Socket对象
        Socket operator = (Socket&& other) noexcept {
            if (this != &other) {
                close();
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }
    };
}