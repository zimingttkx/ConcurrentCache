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

        if (revents & EPOLLERR) {
            if (error_cb) error_cb();
        }
        if (revents & (EPOLLHUP | EPOLLRDHUP)) {
            if (close_cb) close_cb();
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
