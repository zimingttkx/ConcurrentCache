#include "channel.h"
#include "event_loop.h"

namespace cc_server {
    Channel::Channel(EventLoop* loop, int fd)
        : loop_(loop),
          fd_(fd),
          events_(0),
          triggered_events_(0)
    {}

    void Channel::set_read_callback(ReadCallback cb) {
        read_cb_ = std::move(cb);
    }

    void Channel::set_write_callback(WriteCallback cb) {
        write_cb_ = std::move(cb);
    }

    void Channel::set_error_callback(ErrorCallback cb) {
        error_cb_ = std::move(cb);
    }

    void Channel::set_close_callback(CloseCallback cb) {
        close_callback_ = std::move(cb);
    }

    void Channel::enable_reading() {
        events_ |= EPOLLIN;
        update();
    }

    void Channel::enable_writing() {
        events_ |= EPOLLOUT;
        update();
    }

    void Channel::disable_all() {
        events_ = 0;
        update();
    }

    void Channel::handle_event() {
        auto read_cb  = read_cb_;
        auto write_cb = write_cb_;
        auto error_cb = error_cb_;
        auto close_cb = close_callback_;

        uint32_t revents = triggered_events_;
        triggered_events_ = 0;

        // EPOLLERR / EPOLLHUP / EPOLLRDHUP 都表示连接已不可用，应触发关闭。
        // 注意：EPOLLHUP 常与 EPOLLIN 同时上报（对端关闭），若只绑 error_cb 而
        // close_cb 为空，HUP 会被静默忽略 → 连接泄漏（修复 P2-3）。
        if (revents & EPOLLERR) {
            if (error_cb) error_cb();
            return;  // 错误后不再处理读/写，交由 close 路径统一清理
        }
        if (revents & (EPOLLHUP | EPOLLRDHUP)) {
            if (close_cb) close_cb();
            return;  // HUP 同样终结该连接的处理，避免对已半关的 fd 继续读写
        }
        if (revents & EPOLLIN) {
            if (read_cb) read_cb();
        }
        if (revents & EPOLLOUT) {
            if (write_cb) write_cb();
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

    void Channel::update() {
        loop_->update_channel(this);
    }
}
