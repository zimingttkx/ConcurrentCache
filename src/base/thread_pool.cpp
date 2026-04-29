//
// thread_pool.cpp
// ThreadPool 线程池实现
//

#include "thread_pool.h"
#include "log.h"
#include <algorithm>

namespace cc_server {

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false), active_threads_(0), submitted_tasks_(0) {

    // 线程数量至少为 1
    if (num_threads == 0) {
        num_threads = 1;
    }

    // 创建工作线程
    // 注意：线程在创建后立即开始运行 worker_loop()
    threads_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this, i]() {
            worker_loop(i);
        });
    }

    LOG_INFO(THREAD_POOL, "ThreadPool created with %zu workers", num_threads);
}

ThreadPool::~ThreadPool() {
    // RAII：析构时自动停止
    // 即使用户忘记调用 stop()，线程也会安全退出
    stop();
}

void ThreadPool::stop() {
    LOG_INFO(THREAD_POOL, "ThreadPool stopping...");

    // 设置停止标志
    bool expected = false;
    if (stop_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        cv_.notify_all();
    }

    // 等待所有工作线程结束
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    LOG_INFO(THREAD_POOL, "ThreadPool stopped, %lu tasks submitted",
             (unsigned long)submitted_tasks_.load());
}

void ThreadPool::worker_loop(size_t thread_id) {
    LOG_DEBUG(THREAD_POOL, "Worker thread %zu started", thread_id);

    while (true) {
        std::unique_ptr<ThreadPool::Task> task = take_task();

        if (task == nullptr) {
            break;
        }

        active_threads_.fetch_add(1, std::memory_order_relaxed);
        LOG_DEBUG(THREAD_POOL, "Worker %zu executing task", thread_id);

        try {
            task->run();
        } catch (const std::exception& e) {
            LOG_ERROR(THREAD_POOL, "Worker %zu task exception: %s",
                      thread_id, e.what());
        } catch (...) {
            LOG_ERROR(THREAD_POOL, "Worker %zu unknown task exception", thread_id);
        }

        active_threads_.fetch_sub(1, std::memory_order_relaxed);
        LOG_DEBUG(THREAD_POOL, "Worker %zu task completed", thread_id);
    }

    LOG_DEBUG(THREAD_POOL, "Worker thread %zu exited", thread_id);
}

std::unique_ptr<ThreadPool::Task> ThreadPool::take_task() {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // 等待条件：队列非空 或 已停止
    // - 队列非空：可以取任务
    // - 已停止且队列为空：应该退出
    cv_.wait(lock, [this]() {
        return !tasks_.empty() || stop_.load(std::memory_order_acquire);
    });

    // 已停止且队列为空，直接返回
    if (stop_.load(std::memory_order_acquire) && tasks_.empty()) {
        return nullptr;
    }

    // 取出一个任务
    std::unique_ptr<ThreadPool::Task> task = std::move(tasks_.front());
    tasks_.pop();

    return task;
}

size_t ThreadPool::queue_size() const {
    // 只在读取时加锁，不需要太严格
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

} // namespace cc_server
