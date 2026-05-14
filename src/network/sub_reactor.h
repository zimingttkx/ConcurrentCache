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
    static std::unique_ptr<SubReactor> create();
    ~SubReactor();
    SubReactor(const SubReactor&) = delete;
    SubReactor& operator=(const SubReactor&) = delete;
    void start();
    void stop();
    void stop_without_join();
    void join_thread();
    void add_connection(int client_fd);
    EventLoop* event_loop() const { return loop_.get(); }
    [[nodiscard]]size_t connection_count() const {
        return connection_count_.load(std::memory_order_relaxed);
    }
private:
    SubReactor();
    static void handle_read(Connection* conn);
    static void handle_write(Connection* conn);
    void handle_close(Connection* conn);
    void remove_connection(Connection* conn);
    std::unique_ptr<EventLoop> loop_;
    std::atomic<std::thread*> thread_{nullptr};
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    mutable std::shared_mutex connections_mutex_;
    std::atomic<size_t> connection_count_;
};

}
#endif
