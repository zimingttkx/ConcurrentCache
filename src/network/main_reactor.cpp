//
// main_reactor.cpp
// MainReactor实现
//

#include "main_reactor.h"
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
    // ========== 步骤1：创建并初始化listen socket ==========
    if (!listen_socket_.bind_and_listen(port)) {
        LOG_ERROR(NETWORK, "MainReactor failed to create listen socket on port %d", port);
        return false;
    }

    // 设置为非阻塞（必须！否则epoll_wait会阻塞整个线程）
    int flags = fcntl(listen_socket_.fd(), F_GETFL, 0);
    fcntl(listen_socket_.fd(), F_SETFL, flags | O_NONBLOCK);

    // ========== 步骤2：创建Channel监听accept事件 ==========
    listen_channel_ = std::make_unique<Channel>(loop_.get(), listen_socket_.fd());

    // 设置回调：当listen socket可读时（=有新连接），调用handle_accept
    listen_channel_->set_read_callback([this]() {
        handle_accept();
    });

    // 监听读事件（accept就是读事件）
    listen_channel_->enable_reading();

    // ========== 步骤3：注册到EventLoop ==========
    loop_->update_channel(listen_channel_.get());

    initialized_ = true;
    LOG_INFO(NETWORK, "MainReactor initialized, listening on port %d", port);
    return true;
}

void MainReactor::start() {
    if (!initialized_) {
        LOG_ERROR(NETWORK, "MainReactor not initialized, cannot start");
        return;
    }

    running_ = true;
    LOG_INFO(NETWORK, "MainReactor starting...");

    // 启动事件循环（阻塞）
    loop_->loop();
}

void MainReactor::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    loop_->quit();
}

void MainReactor::handle_accept() {
    // ========== 循环accept所有新连接 ==========
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
    // ========== 步骤1：设置非阻塞 ==========
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // ========== 步骤2：获取下一个SubReactor（负载均衡） ==========
    SubReactor* target_reactor = SubReactorPool::instance().get_next_reactor();

    // ========== 步骤3：在SubReactor线程中添加连接 ==========
    // 这里需要思考：SubReactor的EventLoop在独立线程中
    // 我们如何安全地添加连接？
    //
    // 方案：使用EventLoop的wakeup机制
    // 1. 往SubReactor的EventLoop添加一个任务
    // 2. wakeup SubReactor让它处理这个任务
    //
    // 但我们目前的实现比较简单：直接调用（因为SubReactor还没启动时我们不会accept）
    // 更好的做法是用pending queue + wakeup

    // 直接调用add_connection（简化版本）
    // 注意：这样是在MainReactor线程调用的
    // 如果要完全无锁，需要用pending queue
    target_reactor->add_connection(client_fd);

    LOG_INFO(NETWORK, "New connection assigned to SubReactor, fd=%d", client_fd);
}

} // namespace cc_server
