//
// main_reactor.cpp
// MainReactor实现
//

#include "main_reactor.h"
#include "base/server_stats.h"
#include "sub_reactor_pool.h"
#include "base/log.h"

namespace cc_server {

MainReactor::MainReactor()
    : loop_(std::make_unique<EventLoop>()) {
}

MainReactor::~MainReactor() {
    stop();
}

bool MainReactor::init(int port) {
    // 步骤1：创建并初始化listen socket
    if (!listen_socket_.bind_and_listen(port)) {
        LOG_ERROR(NETWORK, "MainReactor failed to create listen socket on port %d", port);
        return false;
    }

    // 设置为非阻塞（必须！否则epoll_wait会阻塞整个线程）
    int flags = fcntl(listen_socket_.fd(), F_GETFL, 0);
    fcntl(listen_socket_.fd(), F_SETFL, flags | O_NONBLOCK);

    // 步骤2：创建Channel监听accept事件
    listen_channel_ = std::make_unique<Channel>(loop_.get(), listen_socket_.fd());

    // 设置回调：当listen socket可读时（=有新连接），调用handle_accept
    listen_channel_->set_read_callback([this]() {
        handle_accept();
    });

    // 监听读事件（accept就是读事件）
    listen_channel_->enable_reading();

    // 步骤3：注册到EventLoop
    loop_->update_channel(listen_channel_.get());

    initialized_.store(true, std::memory_order_release);
    LOG_INFO(NETWORK, "MainReactor initialized, listening on port %d", port);
    return true;
}

void MainReactor::start() {
    if (!initialized_.load(std::memory_order_acquire)) {
        LOG_ERROR(NETWORK, "MainReactor not initialized, cannot start");
        return;
    }

    running_.store(true, std::memory_order_release);
    LOG_INFO(NETWORK, "MainReactor starting...");

    // 启动事件循环（阻塞）
    loop_->loop();
}

void MainReactor::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);
    loop_->quit();
}

void MainReactor::handle_accept() {
    // 循环accept所有新连接
    // 为什么用循环？
    // - epoll触发一次可能意味有多个连接等待
    // - 循环accept直到EAGAIN（没有更多连接）
    while (true) {
        int client_fd = listen_socket_.accept();
        if (client_fd < 0) {
            // EAGAIN或EWOULDBLOCK：没有更多连接了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 其他错误
            LOG_ERROR(NETWORK, "accept failed, errno=%d", errno);
            break;
        }

        // 成功accept一个连接
        add_new_connection(client_fd);
    }
}

void MainReactor::add_new_connection(int client_fd) {
    // 只在真正接管这条连接之后计数：accept 失败、EAGAIN 都不算。
    ServerStats::instance().record_connection_accepted();

    // 步骤1：设置非阻塞
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // 步骤2：获取下一个SubReactor（负载均衡）
    SubReactor* target_reactor = SubReactorPool::instance().get_next_reactor();

    // 步骤3：在SubReactor线程中添加连接
    // 这里需要思考：SubReactor的EventLoop在独立线程中
    // 我们如何安全地添加连接？
    //
    // 方案：使用EventLoop的wakeup机制
    // 1. 往SubReactor的EventLoop添加一个任务
    // 2. wakeup SubReactor让它处理这个任务
    //
    // 但我们目前的实现比较简单：直接调用（因为SubReactor还没启动时我们不会accept）
    // 更好的做法是用pending queue + wakeup

    // 上面的注释描述的是旧实现。现在 add_connection 不再就地登记：它把
    // "建 Connection、加进 epoll、放进 connections_" 整段投递给 SubReactor
    // 自己的线程去做（EventLoop::queue_in_loop + wakeup pipe），accept 线程
    // 从此不碰 epoll，也不存在"事件跑到登记前面"的窗口。
    if (!target_reactor) {
        // 线程池已停却还在 accept，通常是关闭时序问题；fd 必须自己收掉。
        LOG_ERROR(NETWORK, "No SubReactor available for fd=%d, closing it", client_fd);
        ::close(client_fd);
        return;
    }

    target_reactor->add_connection(client_fd);

    LOG_INFO(NETWORK, "New connection assigned to SubReactor, fd=%d", client_fd);
}

} // namespace cc_server
