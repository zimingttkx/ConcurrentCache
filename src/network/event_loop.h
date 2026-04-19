#ifndef CONCURRENTCACHE_NETWORK_EVENT_LOOP_H
#define CONCURRENTCACHE_NETWORK_EVENT_LOOP_H

#include <sys/epoll.h> // linux系统调用文件
#include <unistd.h> // 关闭文件描述符 创建管道用于跨线程通信
#include <vector> // 动态分配避免频繁拷贝
#include <unordered_map> // O(1)查找fd对应的Channel
#include <cstring>
#include <cerrno>
#include "channel.h"
#include "base/log.h"

namespace cc_server {
    // 前向声明 暂时不做完整定义
    class Channel;
    /**
       * @brief EventLoop类：单Reactor模式的事件循环核心（高并发服务器核心组件）
       *
       * 作用：
       *
  EventLoop是整个服务器的事件驱动引擎，它统一管理所有文件描述符（socket连接）的事件监听和分发。
       *
       * 工作原理（类比）：
       * - epoll就像一个"前台接待员"，管理所有客户的"来访登记"
       * - 你告诉接待员："等用户A来电话时叫我，用户B可以写信时叫我"
       * - 接待员一直等待（epoll_wait），有客户来时才通知你
       * - 你不需要一直polling问"有没有人来"，而是被动等待通知
       *
       * 核心职责：
       * 1. 创建和管理epoll实例（epoll_create1）
       * 2. 添加/删除/更新要监控的Channel
       * 3. 阻塞等待事件发生（epoll_wait）
       * 4. 事件就绪时，分发给对应的Channel处理
       *
       * 为什么要wakeup pipe？
       * - EventLoop在epoll_wait中阻塞等待
       * - 当其他线程想让主线程做某件事时（如处理紧急任务）
       * - 其他线程向pipe写端写入数据，epoll_wait立即返回
       * - 这是一种跨线程通信机制
       *
       * 设计要点：
       * - 单例：一个进程只有一个EventLoop（骨架版本简化）
       * - 非拷贝：禁止复制EventLoop对象
       * - 线程安全：EventLoop本身只会被一个线程调用
       */

    class EventLoop {
    private:
        int epoll_fd_; // 创建后默认初始化为-1表示无效状态 创建成功后赋值为有效的fd
        int wakeup_fd_;
        int wakeup_pipe_[2]; // 用于跨线程唤醒的管道，wakeup_pipe_[0]读端，wakeup_pipe_[1]写端
        bool quit_; // 退出标志 控制事件循环是否继续运行
        /**
           * @brief 就绪事件数组（epoll_wait的输出参数）
           *
           * struct epoll_event是什么？
           * - Linux结构体，用于存储已就绪的文件描述符和发生的事件类型
           * epoll_wait返回时，events_数组中前n个元素是就绪的事件
           */
        std::vector<struct epoll_event> events_;
        /**
           * @brief 文件描述符到Channel对象的映射表（哈希表）
           *
           * 为什么要用unordered_map？
           * - 每次epoll_wait返回时，只知道"哪个fd就绪了"
           * - 但处理事件需要调用对应的Channel对象
           * - 这个映射表就是 fd -> Channel* 的快速查找
           * 哈希表键值：fd（文件描述符，整数）
           * 哈希表值：Channel*（指向该fd对应的Channel对象）
           */
          std::unordered_map<int, Channel*> channels_;


    public:
        // 构造函数 创建epoll实例和wakeup_pipe
        EventLoop();
        ~EventLoop();

        // 禁止拷贝
        EventLoop(const EventLoop&) = delete;
        EventLoop& operator = (const EventLoop&) = delete;

        // ==================== 核心功能：事件循环 ====================
        /**
         * @brief 启动事件循环（主循环）
         *
         * 这是整个服务器的"引擎"，会一直运行直到quit_被设置为true
         *
         * 循环内部做的事情：
         * 1. epoll_wait() 阻塞等待事件就绪（阻塞100ms）
         * 2. 遍历所有就绪的事件
         * 3. 如果是wakeup pipe，直接消费数据（handle_wakeup）
         * 4. 如果是普通fd，找到对应的Channel，调用其handle_event()
         *
         * 什么时候会退出？
         * - quit()被调用，设置quit_ = true
         * - 收到SIGINT/SIGTERM信号
         */

        void loop();

        /**
           * @brief 退出事件循环
           *
           * 如何实现优雅退出？
           * 1. 设置quit_ = true
           * 2. 调用wakeup()向pipe写数据，唤醒epoll_wait
           * 3. epoll_wait返回后，循环检查quit_ = true，退出while循环
           *
           * 为什么需要wakeup()？
           * - 如果不wakeup，epoll_wait可能在quit_被设置前一直阻塞
           * - 虽然epoll_wait有100ms超时，但主动wakeup更及时
           */

        void quit();

        // ==================== Channel管理 ====================
        /**
         * @brief 添加新Channel或更新已有Channel到epoll
         *
         * @param channel 要添加/更新的Channel指针，不能为空
         *
         * 添加流程：
         * 1. 获取Channel的fd和要监听的事件
         * 2. 创建epoll_event结构体
         * 3. 调用epoll_ctl(ADD)将fd加入epoll监听
         *
         * 更新流程（Channel已存在时）：
         * 1. 获取Channel当前要监听的事件
         * 2. 调用epoll_ctl(MOD)修改已存在fd的事件
         *
         * 为什么Channel要区分"添加"和"更新"？
         * - 新连接：Channel刚创建，fd还没加入epoll，需要ADD
         * - 已连接：Channel的监听事件可能变化（如从监听读改为监听写），需要MOD
         */
         void update_channel(Channel* channel);
       /**
         * @brief 从epoll中移除Channel
         *
         * @param channel 要移除的Channel指针
         *
         * 移除时机：
         * - 客户端断开连接时
         * - Connection关闭时调用
         *
         * 做了哪些事？
         * 1. 找到fd对应的Channel
         * 2. 调用epoll_ctl(DEL)从epoll中删除
         * 3. 从channels_映射表中移除
         */
       void remove_channel(Channel* channel);

       // ==================== 跨线程唤醒 ====================
       /**
        * @brief 唤醒EventLoop（用于其他线程通知主线程）
        *
        * 什么时候需要唤醒？
        * - 其他线程想让EventLoop处理紧急任务
        * - quit()被调用，需要EventLoop立即退出
        *
        * 如何实现？
        * - 向wakeup_pipe_[1]（写端）写入一个字节的数据
        * - epoll_wait检测到wakeup_fd_可读，立即返回
        * - handle_wakeup()消费这些数据
        */
        void wakeup();

    private:
        /**
           * @brief 处理wakeup事件（消费pipe中的数据）
           *
           * 为什么需要这个函数？
           * - wakeup()写入pipe的数据只是"信号"，不是真实数据
           * - 如果不消费，下次epoll_wait还会认为wakeup_fd_可读
           * - 所以handle_wakeup要读取并丢弃这些数据
           *
           * 读取策略：
           * - 循环读取直到没有数据（read返回EAGAIN/EWOULDBLOCK）
           * - 读端设置O_NONBLOCK，没有数据时立即返回，不会阻塞
           */
        void handle_wakeup();


        /**
           * @brief 创建wakeup pipe
           *
           * 创建流程：
           * 1. pipe(wakeup_pipe_) 创建匿名管道
           * 2. wakeup_pipe_[0]是读端，赋给wakeup_fd_
           * 3. 设置写端为非阻塞（O_NONBLOCK）
           *
           * 返回值：
           * - 成功返回true
           * - 失败返回false，错误信息通过LOG_ERROR输出
           */
        bool create_wakeup_pipe();

    };
}

#endif