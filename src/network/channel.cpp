#include "channel.h"
#include "event_loop.h"

namespace cc_server {
    // 构造函数：初始化各成员变量
    Channel::Channel(EventLoop* loop, int fd)
        : loop_(loop),
          fd_(fd),
          events_(0),
          triggered_events_(0)
    {}

    // 设置可读事件回调
    void Channel::set_read_callback(ReadCallback cb) {
        read_cb_ = std::move(cb);
    }

    // 设置可写事件回调
    void Channel::set_write_callback(WriteCallback cb) {
        write_cb_ = std::move(cb);
    }

    // 设置错误事件回调
    void Channel::set_error_callback(ErrorCallback cb) {
        error_cb_ = std::move(cb);
    }

    // 启用读事件：位或 EPOLLIN 到 events_，调用 update() 注册到 epoll
    void Channel::enable_reading() {
        events_ |= EPOLLIN;
        update();
    }

    // 启用写事件：位或 EPOLLOUT 到 events_，调用 update() 注册到 epoll
    void Channel::enable_writing() {
        events_ |= EPOLLOUT;
        update();
    }

    // 禁用所有事件：events_ 清零，调用 update() 更新 epoll
    void Channel::disable_all() {
        events_ = 0;
        update();
    }

    // 事件处理：根据 triggered_events_ 判断事件类型，调用对应回调
    void Channel::handle_event() {
        if (triggered_events_ & EPOLLIN) {
            if (read_cb_) {
                read_cb_();
            }
        }

        if (triggered_events_ & EPOLLOUT) {
            if (write_cb_) {
                write_cb_();
            }
        }

        if (triggered_events_ & (EPOLLERR | EPOLLHUP)) {
            if (error_cb_) {
                error_cb_();
            }
        }
    }

    int Channel::fd() const {
        return fd_;
    }

    uint32_t Channel::events() const {
        return events_;
    }

    void Channel::set_triggered_events(uint32_t events) {
        triggered_events_ = events;
    }
}