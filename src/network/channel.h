#ifndef CONCURRENT_NETWORK_CHANNEL_H
#define CONCURRENT_NETWORK_CHANNEL_H

#include <cstdint>
#include <functional>  // 用于 std::function，存储回调函数（可以存一个函数）
#include <sys/epoll.h> // Linux epoll 系统调用头文件，定义 EPOLLIN/EPOLLOUT 等事件

/**
 * @brief Channel 通道类（Reactor 高并发服务器核心组件）
 *
 * 作用：
 * 一个 Channel 专门管一个文件描述符（socket/连接），负责告诉 epoll 要监听什么事件，
 * 事件触发后，调用你提前设置好的回调函数（读/写/错误处理）。
 *
 * 核心分工（：
 * 1. EventLoop：只负责等待 epoll 事件，不处理业务逻辑
 * 2. Channel：只负责事件分发，判断事件类型 + 调用回调函数
 *
 * 为什么要用回调？
 * 因为不知道事件什么时候发生（客户端何时发数据、何时可写），
 * 所以先把“要做什么”存起来，等事件来了再调用。
 *
 * 为什么要存回调？
 * 因为设置函数和调用函数不在同一个地方、不在同一个时间，
 * 不存起来，后面就找不到这个函数了（函数参数会被销毁）。
 */

namespace cc_server {
    // 前向声明：告诉编译器 EventLoop 是一个类，解决头文件循环依赖问题
    class EventLoop;

    class Channel {
    private:
        // 所属的事件循环，一个 Channel 只能属于一个 EventLoop
        // EventLoop 负责等待事件，然后通知当前 Channel 处理
        EventLoop* loop_;

        // 当前 Channel 管理的文件描述符（socket 连接、监听 fd 等）
        // 一个 Channel 永远只管理一个 fd
        int fd_;

        // 用户要监听的事件
        // 比如：我想监听可读事件 EPOLLIN，可写事件 EPOLLOUT
        // 由 enable_reading() / enable_writing() 设置
        uint32_t events_;

        // 内核 epoll 实际触发的事件
        // 由 EventLoop 检测到事件后，调用 set_triggered_events() 设置进来
        // 这是真正发生的事情，必须用它来判断事件类型！
        uint32_t triggered_events_;

        // ===================== 回调函数成员变量=====================
        // 作用：存储外部传进来的函数，现在不调用，等事件来了再调用
        // 为什么要存？因为设置函数和调用函数不在同一个时间、同一个函数里
        ReadCallback  read_cb_;   // 可读事件触发时调用
        WriteCallback write_cb_;  // 可写事件触发时调用
        ErrorCallback error_cb_;  // 错误/断开连接时调用

    public:
        // 定义回调函数类型：无参数、无返回值的函数
        // 外部可以把自己的业务函数传给 Channel 存储
        using ReadCallback  = std::function<void()>;
        using WriteCallback = std::function<void()>;
        using ErrorCallback = std::function<void()>;

        // 构造函数：绑定所属 EventLoop 和 要管理的 fd
        Channel(EventLoop* loop, int fd)
            : loop_(loop),
              fd_(fd),
              events_(0),       // 默认不监听任何事件
              triggered_events_(0)
        {}

        // 默认析构函数
        ~Channel() = default;

        // 禁止拷贝和赋值
        // 一个 Channel 只能管理一个 fd，属于一个 EventLoop，不能复制
        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        // ===================== 设置回调函数=====================
        // 作用：把外部的业务函数存到 Channel 内部，将来调用
        // 参数 cb：外部传进来的函数（回调）
        // std::move：高效转移函数对象，不拷贝，高性能

        // 设置【可读事件】回调
        // 场景：客户端发数据 -> 触发 EPOLLIN -> 调用这个函数
        void set_read_callback(ReadCallback cb) {
            read_cb_ = std::move(cb);
        }

        // 设置【可写事件】回调
        // 场景：可以发送数据给客户端 -> 触发 EPOLLOUT -> 调用这个函数
        void set_write_callback(WriteCallback cb) {
            write_cb_ = std::move(cb);
        }

        // 设置【错误/断开】事件回调
        // 场景：连接异常断开、出错 -> 调用这个函数
        void set_error_callback(ErrorCallback cb) {
            error_cb_ = std::move(cb);
        }

        // ===================== 设置要监听的事件=====================
        // 启用【可读事件】监听
        // 告诉 epoll：这个 fd 有数据来的时候，通知我
        void enable_reading() {
            // 位或操作：把 EPOLLIN 加入监听事件列表，不影响其他已设置的事件
            events_ |= EPOLLIN;
            // 更新到内核 epoll 实例，让设置生效
            update();
        }

        // 启用【可写事件】监听
        // 告诉 epoll：这个 fd 可以发送数据的时候，通知我
        void enable_writing() {
            events_ |= EPOLLOUT;
            update();
        }

        // 禁用所有事件监听
        // 告诉 epoll：不要再管这个 fd 了
        void disable_all() {
            events_ = 0;
            update();
        }

        // ===================== 核心：事件处理函数（你最常问的函数）=====================
        // 调用者：EventLoop（当 epoll 检测到事件时调用）
        // 作用：判断触发了什么事件，调用对应的回调函数
        void handle_event() {
            // ===================== 第一层 if：判断【发生了什么事件】=====================
            // 必须用 triggered_events_（实际触发的事件）
            // 不能用 events_ 那是想监听的事件

            // 1. 可读事件触发（有数据来了）
            if (triggered_events_ & EPOLLIN) {
                // ===================== 第二层 if：判断【有没有设置回调函数】=====================
                // 如果用户没设置回调，就不调用，防止程序崩溃
                // 没设置代表不关心这个事件，属于正常情况，不打印日志
                if (read_cb_) {
                    read_cb_(); // 调用外部传进来的业务函数
                }
            }

            // 2. 可写事件触发（可以发送数据了）
            if (triggered_events_ & EPOLLOUT) {
                if (write_cb_) {
                    write_cb_();
                }
            }

            // 3. 错误事件 / 连接挂断（异常断开、出错）
            if (triggered_events_ & (EPOLLERR | EPOLLHUP)) {
                if (error_cb_) {
                    error_cb_();
                }
            }
        }

        // 声明更新函数：具体实现在 .cc 文件中
        // 作用：把当前 Channel 的监听事件更新到内核 epoll
        void update();

        // ===================== 工具函数（获取内部成员）=====================
        // 获取当前管理的文件描述符
        int fd() const { return fd_; }

        // 获取当前监听的事件
        uint32_t events() const { return events_; }

        // 设置【实际触发的事件】
        // 由 EventLoop 调用，把内核返回的事件传给 Channel
        void set_triggered_events(uint32_t events) {
            triggered_events_ = events;
        }
    };
}

#endif