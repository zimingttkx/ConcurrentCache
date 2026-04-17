#ifndef CONCURRENTCACHE_NETWORK_SOCKET_H
#define CONCURRENTCACHE_NETWORK_SOCKET_H

#include <string>
#include <sys/socket.h> // socket() bind() listen() accept()系统调用 系统核心调用
#include <netinet/in.h> // 网络地址结构体
#include <unistd.h> // close()系统调用
#include <errno.h>
#include <fcntl.h>
#include <cstring> // C字符串操作 初始化地址结构体 strlen获取长度
#include <arpa/inet.h> // inet_ntop() inet_pton()系统调用

#include "../base/log.h"
/*
 *这个文件封装了Linux原生的Socket,bind,listen, accept等系统调用 实现RAII自动管理资源 非阻塞模式 地址复用
 * 是网络通信的基础组件
 *
 *
 */

namespace cc_server {
    /*@brief Socket类: 封装一个Tcp套接字的完整生命周期
     * 1. RAII 自动资源管理 对象销毁的时候自动关闭套接字 避免文件描述符泄露
     * 2. 禁止拷贝 只能移动 确保一个文件描述符只被一个对象管理 防止重复关闭
     * 3. 创建socket直接设置非阻塞模式 适配高并发
     * 4. 错误处理 关键系统调用以及错误日志 资源释放
     */
    class Socket {
    private:
        int fd_; // 核心成员变量 linux文件描述符 代表一个TCP套接字 初始化值为-1 表示无效状态
        // 创建socket之后被赋值为有效的文件描述符
    public:
        // 构造 析构函数 RAII实现
        /*@brief : 默认构造函数创建一个无效的socket对象
         * 初始化文件描述符为-1(-1是约定值 表示无效文件描述符) 代表当前对象没有任何关联的TCP套接字
         * 后序调用bind_and_listen()创建并且绑定监听socket 或者带fd的构造函数包装已有的socket
         */
        Socket() : fd_(-1){}

        // 显示构造函数 使用已有的fd创建socket对象 这个fd一般是accept()返回的客户端链接fd
        // explicit关键字确保隐式类型转换
        explicit Socket(int fd) : fd_(fd) {};

        // 析构函数 自动关闭套接字 实现RAII
        // 对象生命周期结束自动调用close() 确保套接字一定被关闭
        ~Socket(){close();}

        // 禁止拷贝函数和构造赋值
        /*
         * 文件描述符fd_是独占资源 不能被多个socket对象同时管理
         * 如果允许拷贝 那么两个对象会持有同一个fd_ 在析构的时候会导致重复关闭fd_的问题
         * delete关键字可以删除编译器自动生成的拷贝版本
         */
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        // 移动构造: 将一个Socket对象的所有权转移到新的Socket对象而不是拷贝
        /*
         *@param other: 要转移的源Socket对象
         * noexcept 表示这个函数不会抛出异常 STL容器更加高效
         * 场景：比如在函数内创建Socket对象并返回给外部使用，拷贝已被禁止，只能通过移动语义转移所有权
         * 操作流程：
         * 1. 接管源对象的文件描述符fd_
         * 2. 将源对象的fd_设为-1，防止源对象析构时重复关闭套接字
         */

        Socket(Socket&& other) noexcept : fd_(other.fd_) {
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

        Socket& operator = (Socket&& other) noexcept {
            if (this != &other) {
                close();
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        // --------------服务器核心功能 创建监听Socket-------------------------
        /**
     * @brief 创建TCP监听套接字，完成创建->绑定->监听的完整流程
     * @param port 要监听的端口号（0-65535，建议用1024以上的非特权端口）
     * @param reuse 是否开启地址复用（默认开启，解决服务器重启端口占用问题）
     * @return 成功返回true，失败返回false（错误信息通过日志输出）
     *
     * 流程对应TCP服务器标准步骤：socket() -> setsockopt() -> bind() -> listen()
     * 额外优化：原生非阻塞创建，适配高并发服务器模型
     */
        bool bind_and_listen(int port, bool reuse = true) {
            // 1. 创建TCP套接字 设置为非阻塞模式
            // AF_INET: IPv4协议族，IP层协议
            // SOCK_STREAM: TCP协议，面向连接、可靠的字节流服务，传输层TCP
            // SOCK_NONBLOCK: 直接创建非阻塞套接字，后续accept/recv不会阻塞线程
            fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd_ < 0) {
                LOG_ERROR("Socket() create failed: {}", strerror(errno));
                return false;
            }
            // 2. 开启地址复用（SO_REUSEADDR），解决服务器重启时"address already in use"问题
            // 原理：服务器正常关闭时，TCP连接会进入TIME_WAIT状态，系统默认2分钟后才释放端口
            // 开启SO_REUSEADDR后，可直接绑定处于TIME_WAIT状态的端口，避免重启失败
            if (reuse) {
                int opt = 1; // 选项值1表示开启
                // SOL_SOCKET: 套接字级别选项，SO_REUSEADDR: 地址复用选项
                setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            }

            // 准备服务器地址结构体
            sockaddr_in server_addr{};
            server_addr.sin_family = AF_INET; // IPv4协议族 与创建socket时保持一致
            server_addr.sin_addr.s_addr = INADDR_ANY; // 绑定本机所有网卡地址 确保所有IP都能连接到服务器
            server_addr.sin_port = htons(port); // 端口号 htons将主机字节序转化为网络字节序(大端序)
            // 网络连接必须使用大端字节序

            // 4.绑定地址和端口到套接字 将socket和指定ip端口关联起来
            // sockaddr_in是IPv4专用结构体，需要强制转为通用的sockaddr*类型，符合bind()的参数要求
            if (bind(fd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
                LOG_ERROR("Bind() port {} failed: {}", port, strerror(errno));
                close(); // 一旦绑定失败就要关闭创建的socket 避免文件描述符泄露
                return false;
            }
            // 5. 监听端口 将套接字转化为被动模式等待客户端连接
            // 第二个参数表述等待连接队列的最大长度 这里设置为1024
            if (listen(fd_, 1024) < 0) {
                LOG_ERROR("Listen() port {} failed: {}", port, strerror(errno));
                close(); // 一旦监听失败也要关闭创建的socket 避免文件描述符泄露
                return false;
            }
            LOG_INFO("Server started on port {}", port);
            return true;

        }

        // ------------------- 服务器端核心功能：接受客户端连接 -------------------
        /**
         * @brief 接受新的客户端连接，返回客户端的文件描述符
         * @return 成功返回客户端socket的fd，失败返回-1 非阻塞模式下无新连接也返回-1
         *
         * 注意:
         * 1. 使用accept4()替代传统accept()，可以直接设置客户端socket为非阻塞模式
         * 2. 非阻塞模式下，无新连接时会返回-1，errno为EAGAIN/EWOULDBLOCK，这不是错误，只是暂时没有连接
         * 3. 会打印客户端IP地址，方便日志和调试
         */
        int accept() {
            sockaddr_in client_addr{}; // 存储客户端的地址信息 端口 + ip
            socklen_t client_len = sizeof(client_addr); // 地质结构体的长度 accept4会修改这个值

            int client_fd = ::accept4{
                fd_, // 监听套接字的文件描述符
                reinterpret_cast<sockaddr*>(&client_addr), // 存储客户端地址的结构体
                &client_len, // 结构体长度 输入输出参数
                SOCK_NONBLOCK // 设置客户端socket为非阻塞模式}
        };
            if (client_fd < 0){
                // 接受失败或者没有新链接返回-1
                // 非阻塞状态下没有新链接会返回EAGAIN/EWOULDBLOCK 这是正常的
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("Accept() failed: {}", strerror(errno));
                }
                return -1;
            }
            // 打印客户端IP地址 inet_ntoa()将网络字节序IP地址转化为人类可读的点分十进制IP
            LOG_INFO("Client connected: fd={}, ip={}", client_fd, inet_ntoa(client_addr.sin_addr));
            return client_fd;

        };
        // ------------------- 通用功能：数据收发 -------------------
        /**
         * @brief 向套接字发送数据（非阻塞模式）
         * @param buf 要发送的数据缓冲区指针
         * @param len 要发送的数据长度
         * @return 成功返回已发送的字节数，失败返回-1（非阻塞模式下可能返回部分字节）
         *
         * 对应系统调用send()，封装了无效fd的检查，避免对无效socket调用系统调用
         */

        ssize_t send(const void*buf, size_t len) {
            if (fd_ < 0) return -1;
            return ::send(fd_, buf, len, 0); // 调用全局send()系统调用避免和类里面的send重名
        }

            /**
         * @brief 从套接字接收数据（非阻塞模式）
         * @param buf 接收数据的缓冲区指针
         * @param len 缓冲区的最大长度
         * @return 成功返回已接收的字节数，连接关闭返回0，失败返回-1
         *
         * 对应系统调用recv()，封装了无效fd的检查，避免对无效socket调用系统调用
         * 注意：非阻塞模式下，无数据时会返回-1，errno为EAGAIN/EWOULDBLOCK，这不是错误
         */

        ssize_t recv(void* buf, size_t len) {
            if (fd_ < 0) return -1;
            return ::recv(fd_, buf, len, 0); // 调用全局recv()系统调用避免和类里面的recv重名
        }

        // ------------------- 辅助函数 -------------------
            /**
         * @brief 手动关闭套接字（析构函数会自动调用，也可手动调用）
         *
         * 关键安全设计：
         * 1. 仅当fd_ >= 0时才调用close()，避免对无效fd调用系统调用
         * 2. 关闭后将fd_设为-1，标记为无效状态，防止重复关闭
         */
        void close() {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1; // 标记为无效状态防止重复关闭
            }
        }

            /**
         * @brief 获取套接字的文件描述符（只读）
         * @return 当前Socket对象持有的文件描述符，-1表示无效
         *
         * 场景：当需要直接调用系统调用时，比如setsockopt()、fcntl()，需要获取原始fd
         */
        int fd() const {return fd_;};


        /**
         * @brief 判断当前Socket对象是否有效
         * @return 有效返回true（fd_ >= 0），无效返回false
         *
         * 场景：在发送/接收数据前，先判断socket是否有效，避免无效操作
         */
        bool valid() const {return fd_ >= 0;};

    };
}

#endif CONCURRENTCACHE_NETWORK_SOCKET_H