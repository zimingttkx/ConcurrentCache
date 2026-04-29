#include "sub_reactor_pool.h"


namespace cc_server {

    SubReactorPool& SubReactorPool::instance() {
        static SubReactorPool instance;
        return instance;
    }

    void SubReactorPool::init(size_t reactor_count) {
        // 预创建所有SubReactor
        reactors_.reserve(reactor_count);
        for (size_t i = 0; i < reactor_count; ++i) {
            reactors_.push_back(SubReactor::create());
        }
    }

    void SubReactorPool::start() {
        // 启动所有SubReactor（每个运行在独立线程）
        for (auto& reactor : reactors_) {
            reactor->start();
        }
    }

    void SubReactorPool::stop() {
    // 防止重复停止
    if (stopped_.exchange(true)) {
        return;
    }

    // 唤醒所有SubReactor，让它们退出
    for (auto& reactor : reactors_) {
        reactor->stop();
    }
}

    SubReactor* SubReactorPool::get_next_reactor() {
        // 轮询策略
        // atomically fetch and add
        size_t index = next_index_.fetch_add(1, std::memory_order_relaxed);
        return reactors_[index % reactors_.size()].get();
    }

} // namespace cc_server
