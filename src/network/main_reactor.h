//
// main_reactor.h
// MainReactor：只负责处理listen socket，accept新连接
//
// 工作流程：
// 1. 创建listen socket，绑定端口
// 2. 创建Channel监听accept事件
// 3. epoll_wait检测到新连接时，accept并分配给SubReactor
//
// 为什么MainReactor要单独出来？
// - accept是轻量级操作，只需要一个线程处理
// - 如果和SubReactor混在一起，会成为瓶颈
// - 分开后，accept和I/O可以并行
//

#ifndef CONCURRENTCACHE_MAIN_REACTOR_H
#define CONCURRENTCACHE_MAIN_REACTOR_H

#include "event_loop.h"
#include "socket.h"
#include "channel.h"

namespace cc_server {

    class MainReactor {
    public:
        MainReactor();
        ~MainReactor();

        // 禁止拷贝
        MainReactor(const MainReactor&) = delete;
        MainReactor& operator=(const MainReactor&) = delete;

        // 核心功能

        // 初始化listen socket
        // @param port 监听端口
        // @return 是否成功
        bool init(int port);

        // 启动MainReactor（会阻塞）
        void start();

        // 停止MainReactor
        void stop();

        // 获取EventLoop（供外部添加任务）
        EventLoop* event_loop() { return loop_.get(); }

    private:
        // 处理listen socket可读事件（有新连接）
        void handle_accept();

        // 创建并添加新连接
        void add_new_connection(int client_fd);

        // MainReactor自己的EventLoop
        std::unique_ptr<EventLoop> loop_;

        // 监听socket
        Socket listen_socket_;

        // listen socket对应的Channel
        std::unique_ptr<Channel> listen_channel_;

        // 是否已初始化
        bool initialized_{false};

        // 是否正在运行
        bool running_{false};
    };

} // namespace cc_server

#endif // CONCURRENTCACHE_MAIN_REACTOR_H
