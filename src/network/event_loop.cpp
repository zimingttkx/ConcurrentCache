#include "event_loop.h"
#include "channel.h"

namespace cc_server {

    // 构造函数：初始化所有成员
    EventLoop::EventLoop()
        : epoll_fd_(-1),          // 一开始没有epoll，标记为-1表示无效
          wakeup_fd_(-1),         // 一开始没有pipe，初始化-1
          quit_(false) {          // 默认不退出

        // 1. 创建 epoll 实例
        // epoll_create1(0) = 雇一个"前台"
        // 返回值：成功 = epoll的文件描述符（>=0），失败 = -1
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            LOG_ERROR(event_loop, "epoll_create1() failed: %s", strerror(errno));
            return;
        }
        LOG_INFO(event_loop, "epoll created, fd=%d", epoll_fd_);

        // 2. 创建 wakeup pipe（后面会用到）
        // 如果创建失败，要关闭已经创建的 epoll，避免资源泄露
        if (!create_wakeup_pipe()) {
            close(epoll_fd_);
            epoll_fd_ = -1;
            return;
        }

        // 3. 把 pipe 读端交给 epoll 监听
        // 这样其他线程往 pipe 写数据时，epoll_wait 会返回
        struct epoll_event ev;                  // epoll_event 已经在 <sys/epoll.h> 里定义好了
        memset(&ev, 0, sizeof(ev));           // 清零结构体，C++不会自动初始化栈上的变量
        ev.events = EPOLLIN;                   // 监听"可读"事件
        ev.data.fd = wakeup_fd_;             // 存 fd，后面区分是谁

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
            LOG_ERROR(event_loop, "epoll_ctl ADD wakeup_fd_ failed: %s", strerror(errno));
            return;
        }

        // 4. 预分配事件数组，避免每次 loop 都分配内存
        events_.resize(1024);

        LOG_INFO(event_loop, "EventLoop created successfully");
    }

    // 析构函数：程序退出时关闭所有资源
    EventLoop::~EventLoop() {
        // 关闭 epoll
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
        // 关闭 pipe 读端
        if (wakeup_fd_ >= 0) {
            close(wakeup_fd_);
            wakeup_fd_ = -1;
        }
    }

    // 创建 pipe（内部电话）
    bool EventLoop::create_wakeup_pipe() {
        // pipe() 创建一根"水管"
        // pipe[0] = 读端（EventLoop 监听这个）
        // pipe[1] = 写端（其他线程写入来唤醒 EventLoop）
        if (::pipe(wakeup_pipe_) < 0) {
            LOG_ERROR(event_loop, "pipe() failed: %s", strerror(errno));
            return false;
        }

        // 设置写端为非阻塞
        // 为什么写端要非阻塞？
        // 如果 pipe 缓冲区满了（很少见），write 会阻塞
        // 设置 O_NONBLOCK 后，写不进去就立即返回，不会卡住
        int flags = fcntl(wakeup_pipe_[1], F_GETFL, 0);          // 获取当前设置
        fcntl(wakeup_pipe_[1], F_SETFL, flags | O_NONBLOCK);    // 添加非阻塞标志

        wakeup_fd_ = wakeup_pipe_[0];  // 读端交给 EventLoop 监听

        LOG_DEBUG(event_loop, "wakeup pipe created: read=%d, write=%d", wakeup_pipe_[0], wakeup_pipe_[1]);
        return true;
    }

    // 事件循环：启动之后一直跑，死循环等事件、分发事件
    void EventLoop::loop() {
        LOG_INFO(event_loop, "EventLoop start looping...");

        // 循环，直到有人说"别等了"（quit_ = true）
        while (!quit_) {

            // epoll_wait = "前台坐着等，有人来就返回"
            // 等 100ms，看看有没有人来
            int n = epoll_wait(
                epoll_fd_,                           // epoll 文件描述符
                events_.data(),                       // 有人来就存在这里
                static_cast<int>(events_.size()),   // 最多存多少
                100                                  // 等100ms就超时
            );

            // 处理返回值
            if (n < 0) {
                if (errno == EINTR) {
                    continue;  // 被信号打断，继续等
                }
                LOG_ERROR(event_loop, "epoll_wait failed");
                break;  // 真的出错了，退出循环
            }

            if (n == 0) {
                continue;  // 超时，没人来，继续等
            }

            // n > 0 : 有 n 个人来了，遍历处理
            for (int i = 0; i < n; ++i) {
                int fd = events_[i].data.fd;  // 看看是谁的 fd

                // 如果是内部电话（wakeup pipe），说明被其他线程唤醒了
                if (fd == wakeup_fd_) {
                    handle_wakeup();
                    continue;
                }

                // 找对应的 Channel
                auto it = channels_.find(fd);
                if (it != channels_.end()) {
                    // 告诉 Channel 发生了什么事件
                    it->second->set_triggered_events(events_[i].events);
                    // 调用 Channel 的回调函数处理
                    it->second->handle_event();
                }
            }

            // 如果事件数量等于数组大小，可能有很多事件在排队，扩容
            if (n == static_cast<int>(events_.size())) {
                events_.resize(events_.size() * 2);
            }
        }

        LOG_INFO(event_loop, "EventLoop stopped");
    }

    // 退出：设置退出标志，唤醒 loop
    void EventLoop::quit() {
        quit_ = true;   // 设置退出标志
        wakeup();        // 主动唤醒 epoll_wait，让它及时退出
    }

    // 当有新链接到来时，把新的客户端交给 epoll 管理
    void EventLoop::update_channel(Channel* channel) {
        if (!channel) return;

        int fd = channel->fd();
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = channel->events();  // 要监听的事件（读/写）
        ev.data.fd = fd;               // 存 fd

        // 判断是新增还是修改
        auto it = channels_.find(fd);
        if (it == channels_.end()) {
            // 找不到 = 新客户端，ADD（添加）
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
            channels_[fd] = channel;
        } else {
            // 找到了 = 已存在，MOD（修改）
            epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    // 当客户端断开时，从 epoll 移除管理
    void EventLoop::remove_channel(Channel* channel) {
        if (!channel) return;

        int fd = channel->fd();

        auto it = channels_.find(fd);
        if (it != channels_.end()) {
            // 从 epoll 删除
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            // 从映射表移除
            channels_.erase(it);
        }
    }

    // 其他线程想叫醒主线程：往 pipe 写数据，唤醒 epoll_wait
    void EventLoop::wakeup() {
        char one_byte = 'a';
        // 向 pipe 写端写一个字节
        ssize_t n = ::write(wakeup_pipe_[1], &one_byte, sizeof(one_byte));
        if (n < 0) {
            LOG_ERROR(event_loop, "wakeup write() failed");
        }
    }

    // wakeup 被触发时：读取并丢弃 pipe 里面的数据
    void EventLoop::handle_wakeup() {
        // 读取并丢弃 pipe 中的数据
        // 因为这些数据只是"信号"，不是真实数据，不读掉会残留
        char buf[1024];
        while (true) {
            ssize_t n = ::read(wakeup_fd_, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 没数据了，退出
                }
                break;
            }
            if (n == 0) {
                break;  // 写端关闭，退出
            }
            // n > 0：读到数据了，继续读，直到没数据
        }
    }

}
