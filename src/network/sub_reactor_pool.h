#ifndef CONCURRENTCACHE_SUB_REACTOR_POOL_H
#define CONCURRENTCACHE_SUB_REACTOR_POOL_H

#include "sub_reactor.h"
#include <memory>
#include <atomic>

namespace cc_server {
    class SubReactorPool {
    public:
        static SubReactorPool& instance();

        void init(size_t reactor_count);

        void start();

        void stop();

        // 等待所有SubReactor线程结束
        void join_all();

        SubReactor* get_next_reactor();

        [[nodiscard]]size_t size() const{return reactors_.size();}

    private:
        SubReactorPool() = default;

        ~SubReactorPool() = default;

        SubReactorPool(const SubReactorPool&) = delete;
        SubReactorPool& operator=(const SubReactorPool&) = delete;

        std::vector<std::unique_ptr<SubReactor>> reactors_;

        std::atomic<size_t> next_index_{0};

        std::atomic<bool> stopped_{false};
    };
}
#endif //CONCURRENTCACHE_SUB_REACTOR_POOL_H
