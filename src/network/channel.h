#ifndef CONCURRENT_NETWORK_CHANNEL_H
#define CONCURRENT_NETWORK_CHANNEL_H

#include <cstdint>
#include <functional>
#include <sys/epoll.h>

namespace cc_server {
    class EventLoop;

    /**
     * @brief Channel类：文件描述符事件封装
     *
     * 协作关系：
     * - 属于 EventLoop 管理，每个 Channel 关联一个 fd
     * - 保存读/写/错误回调，事件触发时调用对应回调
     * - update() 将监听事件注册到 epoll
     * - EventLoop 检测到事件后调用 handle_event() 分发
     */
    class Channel {
    public:
        using ReadCallback  = std::function<void()>;
        using WriteCallback = std::function<void()>;
        using ErrorCallback = std::function<void()>;

    private:
        EventLoop* loop_;
        int fd_;
        uint32_t events_;          // 要监听的事件
        uint32_t triggered_events_; // 实际触发的事件
        ReadCallback  read_cb_;
        WriteCallback write_cb_;
        ErrorCallback error_cb_;

    public:

        Channel(EventLoop* loop, int fd);
        ~Channel() = default;

        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        // 设置回调
        void set_read_callback(ReadCallback cb);
        void set_write_callback(WriteCallback cb);
        void set_error_callback(ErrorCallback cb);

        // 设置监听事件
        void enable_reading();
        void enable_writing();
        void disable_all();

        // 事件处理
        void handle_event();

        // 更新到 epoll
        void update();

        // 工具函数
        int fd() const;
        uint32_t events() const;
        void set_triggered_events(uint32_t events);
    };
}

#endif