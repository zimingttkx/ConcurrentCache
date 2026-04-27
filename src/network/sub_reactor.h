#ifndef CONCURRENTCACHE_SUB_REACTOR_H
#define CONCURRENTCACHE_SUB_REACTOR_H

#include "event_loop.h"
#include "connection.h"
#include <memory>
#include <shared_mutex>
#include <atomic>

namespace cc_server {

class SubReactor {
public:
    // 工厂方法：创建SubReactor
    static std::unique_ptr<SubReactor> create();

    ~SubReactor();

    // 禁止拷贝
    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;

    // ========== 核心功能 ==========

    // 启动SubReactor的事件循环（在独立线程中运行）
    void start();

    // 停止SubReactor
    void stop();

    // 处理新连接（由MainReactor调用）
    // @param client_fd 已连接的客户端fd
    void add_connection(int client_fd);

    // 获取SubReactor的EventLoop（供MainReactor添加listen socket用）
    EventLoop* event_loop() const { return loop_.get(); }

    // 获取当前连接数量（用于负载均衡）
    [[nodiscard]]size_t connection_count() const {
        return connection_count_.load(std::memory_order_relaxed);
    }

private:
    // 私有构造函数
    SubReactor();

    // 处理连接读事件
    static void handle_read(Connection* conn);

    // 处理连接写事件
    static void handle_write(Connection* conn);

    // 处理连接关闭
    void handle_close(Connection* conn);

    // 移除连接
    void remove_connection(Connection* conn);

    // SubReactor自有的EventLoop（独立的epoll实例）
    std::unique_ptr<EventLoop> loop_;

    // 独立线程，运行事件循环
    std::thread* thread_;

    // 保存这个SubReactor管理的所有Connection
    // key: fd, value: Connection unique_ptr
    // 使用shared_ptr是因为Connection可能被跨线程引用
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;

    // 保护connections_的读写锁 多线程访问
    // mutable 确保const函数也可以修改变量 因为锁本身是需要被修改的
    mutable std::shared_mutex connections_mutex_;

    // 连接计数（原子操作，用于负载均衡）
    std::atomic<size_t> connection_count_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_SUB_REACTOR_H
