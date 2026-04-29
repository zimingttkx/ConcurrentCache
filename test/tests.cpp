#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>
#include <random>
#include <future>
#include <sstream>
#include <string>

#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/thread_pool.h"
#include "base/lock.h"
#include "memorypool/memory_pool.h"

namespace cc_server {
namespace testing {

// ============================================================================
// ThreadPool Tests
// ============================================================================

class ThreadPoolConcurrencyTest {
public:
    ThreadPoolConcurrencyTest(size_t num_threads, size_t test_duration_ms)
        : num_threads_(num_threads)
        , test_duration_ms_(test_duration_ms)
        , submitted_tasks_(0)
        , completed_tasks_(0)
        , expected_completed_(0)
        , running_(true) {
        pool_ = std::make_unique<ThreadPool>(num_threads_);
    }

    ~ThreadPoolConcurrencyTest() {
        stop();
    }

    void run() {
        auto start_time = std::chrono::steady_clock::now();
        std::vector<std::thread> workers;

        for (size_t i = 0; i < num_threads_; ++i) {
            std::string thread_name = "worker_" + std::to_string(i);
            workers.emplace_back([this, i, thread_name]() {
                worker_loop(i, thread_name);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(test_duration_ms_));
        running_ = false;
        pool_->stop();

        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        std::cout << "  Test duration: " << duration << " ms\n";
        std::cout << "  Tasks submitted: " << submitted_tasks_ << "\n";
        std::cout << "  Tasks completed: " << completed_tasks_ << "\n";
    }

private:
    void worker_loop(size_t worker_id, const std::string& thread_name) {
        TRACE_LOG(OpType::THREAD_START, "ThreadPoolConcurrencyTest::worker_loop",
                 "worker_id=" + std::to_string(worker_id));

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> delay_dist(1, 10);

        while (running_) {
            if (submitted_tasks_ < completed_tasks_ + 1000) {
                auto task_id = submitted_tasks_++;
                expected_completed_++;

                TRACE_LOG(OpType::TASK_SUBMIT, "ThreadPoolConcurrencyTest::submit",
                         "task_" + std::to_string(task_id));

                try {
                    pool_->submit([this, task_id]() {
                        execute_task(task_id);
                    });
                } catch (const std::runtime_error&) {
                    // ThreadPool 已停止，忽略
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));
            }
        }

        TRACE_LOG(OpType::THREAD_END, "ThreadPoolConcurrencyTest::worker_loop",
                 "worker_id=" + std::to_string(worker_id));
    }

    int execute_task(int task_id) {
        TRACE_LOG(OpType::TASK_START, "ThreadPoolConcurrencyTest::execute_task",
                 "task_" + std::to_string(task_id));

        int sum = 0;
        for (int i = 0; i < 100; ++i) {
            sum += i;
        }

        completed_tasks_++;

        TRACE_LOG(OpType::TASK_COMPLETE, "ThreadPoolConcurrencyTest::execute_task",
                 "task_" + std::to_string(task_id) + ",result=" + std::to_string(sum));

        return sum;
    }

    void stop() {
        running_ = false;
        if (pool_) {
            pool_->stop();
        }
    }

    size_t num_threads_;
    size_t test_duration_ms_;
    std::unique_ptr<ThreadPool> pool_;
    std::atomic<uint64_t> submitted_tasks_;
    std::atomic<uint64_t> completed_tasks_;
    std::atomic<uint64_t> expected_completed_;
    std::atomic<bool> running_;
};

class ThreadPoolOrderTest {
public:
    ThreadPoolOrderTest(size_t num_threads, size_t num_tasks)
        : num_threads_(num_threads)
        , num_tasks_(num_tasks)
        , task_counter_(0)
        , completed_counter_(0)
        , expected_order_(num_tasks + 1, 0)
        , actual_order_(num_tasks + 1, 0) {
        pool_ = std::make_unique<ThreadPool>(num_threads_);
    }

    void run() {
        std::cout << "  Submitting " << num_tasks_ << " tasks...\n";

        for (size_t i = 0; i < num_tasks_; ++i) {
            size_t order = task_counter_++;
            pool_->submit([this, order]() {
                execute_task(order);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        std::cout << "  Verifying task order...\n";
        size_t errors = 0;
        for (size_t i = 0; i < num_tasks_; ++i) {
            if (expected_order_[i] != actual_order_[i]) {
                errors++;
                if (errors <= 5) {
                    std::cout << "    Order mismatch at task " << i << "\n";
                }
            }
        }

        if (errors > 0) {
            std::cout << "  Order errors: " << errors << "\n";
        } else {
            std::cout << "  All tasks completed in expected order\n";
        }
    }

private:
    void execute_task(size_t order) {
        size_t slot = completed_counter_++;
        actual_order_[slot] = order;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    size_t num_threads_;
    size_t num_tasks_;
    std::unique_ptr<ThreadPool> pool_;
    std::atomic<size_t> task_counter_;
    std::atomic<size_t> completed_counter_;
    std::vector<size_t> expected_order_;
    std::vector<size_t> actual_order_;
};

class ThreadPoolShutdownTest {
public:
    ThreadPoolShutdownTest(size_t num_threads)
        : num_threads_(num_threads)
        , submitted_(0)
        , started_(0)
        , completed_(0) {
        pool_ = std::make_unique<ThreadPool>(num_threads_);
    }

    void run() {
        std::vector<std::thread> submitters;
        std::atomic<bool> stop_flag{false};

        for (size_t i = 0; i < num_threads_; ++i) {
            submitters.emplace_back([this, &stop_flag, i]() {
                while (!stop_flag.load()) {
                    size_t id = submitted_++;
                    pool_->submit([this, id]() {
                        started_++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        completed_++;
                    });

                    if (submitted_ >= 10000) {
                        stop_flag = true;
                        break;
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop_flag = true;

        for (auto& t : submitters) {
            t.join();
        }

        std::cout << "  Submitted: " << submitted_ << ", Started: " << started_
                  << ", Completed: " << completed_ << "\n";

        pool_->stop();
        std::cout << "  After shutdown, completed: " << completed_ << "\n";
    }

private:
    size_t num_threads_;
    std::unique_ptr<ThreadPool> pool_;
    std::atomic<size_t> submitted_;
    std::atomic<size_t> started_;
    std::atomic<size_t> completed_;
};

// ============================================================================
// Lock Tests
// ============================================================================

class CounterConsistencyTest {
public:
    CounterConsistencyTest(int num_threads, int operations_per_thread)
        : num_threads_(num_threads)
        , operations_per_thread_(operations_per_thread)
        , counter_(0)
        , expected_final_(num_threads * operations_per_thread) {}

    void run_with_mutex() {
        std::cout << "  Testing with Mutex...\n";
        counter_ = 0;

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this]() {
                for (int j = 0; j < operations_per_thread_; ++j) {
                    mutex_.lock();
                    int old = counter_.load();
                    counter_.store(old + 1);
                    mutex_.unlock();
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        check_result("Mutex");
    }

    void run_with_spinlock() {
        std::cout << "  Testing with SpinLock...\n";
        counter_ = 0;

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this]() {
                for (int j = 0; j < operations_per_thread_; ++j) {
                    spinlock_.lock();
                    int old = counter_.load();
                    counter_.store(old + 1);
                    spinlock_.unlock();
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        check_result("SpinLock");
    }

    void run_with_atomic() {
        std::cout << "  Testing with Atomic...\n";
        counter_ = 0;

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this]() {
                for (int j = 0; j < operations_per_thread_; ++j) {
                    counter_.fetch_add(1);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        check_result("Atomic");
    }

private:
    void check_result(const std::string& name) {
        int final_value = counter_.load();
        if (final_value == expected_final_) {
            std::cout << "  [" << name << "] Counter correct: " << final_value << "\n";
        } else {
            std::cout << "  [" << name << "] Counter WRONG: expected " << expected_final_
                     << ", got " << final_value << "\n";
        }
    }

    int num_threads_;
    int operations_per_thread_;
    std::atomic<int> counter_;
    int expected_final_;
    std::mutex mutex_;
    SpinLock spinlock_;
};

class DeadlockScenarioTest {
public:
    DeadlockScenarioTest() : shared_data_(0), done_(false) {}

    void run_correct_order() {
        std::cout << "  Testing correct lock ordering (no deadlock)...\n";

        std::thread t1([this]() {
            lock_a_.lock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock_b_.lock();
            shared_data_ = 1;
            lock_b_.unlock();
            lock_a_.unlock();
        });

        std::thread t2([this]() {
            lock_a_.lock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock_b_.lock();
            shared_data_ = 2;
            lock_b_.unlock();
            lock_a_.unlock();
        });

        t1.join();
        t2.join();

        std::cout << "  Completed without deadlock. shared_data=" << shared_data_ << "\n";
    }

    void run_wrong_order_with_timeout() {
        std::cout << "  Testing wrong lock ordering detection...\n";
        std::cout << "  (Using try_lock instead of try_lock_for to avoid template linkage issues)\n";

        std::atomic<bool> deadlock_detected{false};
        std::thread t1([this, &deadlock_detected]() {
            lock_a_.lock();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (lock_b_.try_lock()) {
                shared_data_ = 1;
                lock_b_.unlock();
            } else {
                std::cout << "  [Expected] t1 could not acquire lock_b\n";
                deadlock_detected = true;
            }

            lock_a_.unlock();
        });

        std::thread t2([this, &deadlock_detected]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            lock_b_.lock();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (lock_a_.try_lock()) {
                shared_data_ = 2;
                lock_a_.unlock();
            } else {
                std::cout << "  [Expected] t2 could not acquire lock_a\n";
                deadlock_detected = true;
            }

            lock_b_.unlock();
        });

        t1.join();
        t2.join();

        if (deadlock_detected) {
            std::cout << "  Lock contention scenario detected as expected\n";
        }
    }

private:
    Mutex lock_a_;
    Mutex lock_b_;
    int shared_data_;
    std::atomic<bool> done_;
};

class ShardedLockTest {
public:
    ShardedLockTest(int num_shards, int num_threads, int ops_per_thread)
        : num_shards_(num_shards)
        , num_threads_(num_threads)
        , ops_per_thread_(ops_per_thread)
        , sharded_locks_(num_shards)
        , data_(num_shards, 0) {}

    void run() {
        std::cout << "  Testing with " << num_shards_ << " shards, "
                  << num_threads_ << " threads...\n";

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this, i]() {
                thread_loop(i);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        int64_t total = 0;
        for (const auto& d : data_) {
            total += d;
        }

        int64_t expected = static_cast<int64_t>(num_threads_) * ops_per_thread_;
        std::cout << "  Total operations: " << total << " (expected: " << expected << ")\n";

        if (total == expected) {
            std::cout << "  [OK] All operations counted correctly\n";
        } else {
            std::cout << "  [FAIL] Operation count mismatch\n";
        }
    }

private:
    void thread_loop(int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 999);

        for (int i = 0; i < ops_per_thread_; ++i) {
            std::string key = "key_" + std::to_string(dist(gen) % 1000);
            size_t hash = std::hash<std::string>{}(key);
            size_t shard_idx = hash % num_shards_;

            auto& lock = sharded_locks_.get_shard(key);
            lock.lock();
            data_[shard_idx]++;
            lock.unlock();
        }
    }

    int num_shards_;
    int num_threads_;
    int ops_per_thread_;
    ShardedLock<SpinLock> sharded_locks_;
    std::vector<int> data_;
};

// ============================================================================
// MemoryPool Tests
// ============================================================================

class MemoryPoolConsistencyTest {
public:
    MemoryPoolConsistencyTest(int num_threads, int allocs_per_thread)
        : num_threads_(num_threads)
        , allocs_per_thread_(allocs_per_thread)
        , total_allocated_(0)
        , total_freed_(0)
        , active_allocs_(0) {}

    void run() {
        std::cout << "  Running with " << num_threads_ << " threads, "
                  << allocs_per_thread_ << " allocations each...\n";

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this, i]() {
                thread_loop(i);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        std::cout << "  Total allocated: " << total_allocated_ << "\n";
        std::cout << "  Total freed: " << total_freed_ << "\n";
        std::cout << "  Active allocations: " << active_allocs_ << "\n";

        if (active_allocs_ > 0) {
            std::cout << "  [INFO] Some allocations not freed: " << active_allocs_ << "\n";
        }
    }

private:
    void thread_loop(int thread_id) {
        std::vector<void*> allocations;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> size_dist(8, 256);

        for (int i = 0; i < allocs_per_thread_; ++i) {
            size_t size = size_dist(gen);
            void* ptr = MemoryPool::allocate(size);

            if (ptr != nullptr) {
                allocations.push_back(ptr);
                total_allocated_++;
                active_allocs_++;
            }
        }

        for (void* ptr : allocations) {
            MemoryPool::deallocate(ptr, 128);
            total_freed_++;
            active_allocs_--;
        }
    }

    int num_threads_;
    int allocs_per_thread_;
    std::atomic<uint64_t> total_allocated_;
    std::atomic<uint64_t> total_freed_;
    std::atomic<int64_t> active_allocs_;
};

class MemoryPoolStressTest {
public:
    MemoryPoolStressTest(int num_threads, int duration_ms)
        : num_threads_(num_threads)
        , duration_ms_(duration_ms)
        , running_(true)
        , alloc_count_(0)
        , free_count_(0)
        , live_count_(0) {}

    void run() {
        std::cout << "  Running stress test with " << num_threads_
                  << " threads for " << duration_ms_ << " ms...\n";

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads_; ++i) {
            threads.emplace_back([this, i]() {
                stress_loop(i);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms_));
        running_ = false;

        for (auto& t : threads) {
            t.join();
        }

        std::cout << "  Total allocations: " << alloc_count_ << "\n";
        std::cout << "  Total frees: " << free_count_ << "\n";
        std::cout << "  Live (not freed): " << live_count_ << "\n";
    }

private:
    void stress_loop(int thread_id) {
        std::vector<void*> allocations;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> size_dist(8, 1024);
        std::uniform_int_distribution<> ratio_dist(1, 100);

        while (running_) {
            int ratio = ratio_dist(gen);

            if (ratio <= 60 || allocations.empty()) {
                size_t size = size_dist(gen);
                void* ptr = MemoryPool::allocate(size);
                if (ptr) {
                    allocations.push_back(ptr);
                    alloc_count_++;
                    live_count_++;
                }
            } else {
                size_t idx = allocations.size() - 1;
                void* ptr = allocations[idx];
                allocations.pop_back();

                MemoryPool::deallocate(ptr, 128);
                free_count_++;
                live_count_--;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        for (void* ptr : allocations) {
            MemoryPool::deallocate(ptr, 128);
            free_count_++;
            live_count_--;
        }
    }

    int num_threads_;
    int duration_ms_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> alloc_count_;
    std::atomic<uint64_t> free_count_;
    std::atomic<int64_t> live_count_;
};

class MemoryBoundaryTest {
public:
    MemoryBoundaryTest() {}

    void run() {
        std::cout << "  Testing boundary conditions...\n";

        std::vector<size_t> test_sizes = {
            8, 16, 32, 64, 128, 256,
            1024, 2048, 4096,
            256 * 1024 - 1,
            256 * 1024,
            256 * 1024 + 1
        };

        int success_count = 0;
        int fail_count = 0;

        for (size_t size : test_sizes) {
            void* ptr = MemoryPool::allocate(size);

            if (ptr != nullptr) {
                char* char_ptr = static_cast<char*>(ptr);
                *char_ptr = 0x42;
                if (*char_ptr == 0x42) {
                    MemoryPool::deallocate(ptr, size);
                    success_count++;
                } else {
                    fail_count++;
                }
            } else {
                fail_count++;
            }
        }

        std::cout << "  Success: " << success_count << ", Failures: " << fail_count << "\n";

        if (fail_count == 0) {
            std::cout << "  [OK] All boundary tests passed\n";
        }
    }
};

// ============================================================================
// Test Runners
// ============================================================================

void run_thread_pool_tests() {
    std::cout << "\n========================================\n";
    std::cout << "       ThreadPool Concurrency Tests\n";
    std::cout << "========================================\n\n";

    TraceLogger::instance().initialize("thread_pool_test");

    std::cout << "[Test 1] High Concurrency Stress Test\n";
    ThreadPoolConcurrencyTest test1(4, 2000);
    test1.run();
    std::cout << "\n";

    std::cout << "[Test 2] Task Order Test\n";
    ThreadPoolOrderTest test2(4, 100);
    test2.run();
    std::cout << "\n";

    std::cout << "[Test 3] Shutdown Test\n";
    ThreadPoolShutdownTest test3(8);
    test3.run();
    std::cout << "\n";

    // 分析结果
    TraceAnalyzer analyzer;
    analyzer.add_validator(std::make_shared<ThreadPoolInvariantValidator>(8));

    auto events = TraceLogger::instance().get_events();
    auto lock_infos = TraceLogger::instance().get_lock_infos();
    auto memory_accesses = TraceLogger::instance().get_memory_accesses();

    AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);
    std::cout << report.to_string();

    std::string log_path = TraceLogger::instance().get_log_file_path();
    std::string report_path = log_path.substr(0, log_path.find_last_of('.')) + "_report.txt";
    report.save_to_file(report_path);
    std::cout << "\nReport saved to: " << report_path << "\n";

    TraceLogger::instance().flush_and_close();
}

void run_lock_tests() {
    std::cout << "\n========================================\n";
    std::cout << "          Lock Concurrency Tests\n";
    std::cout << "========================================\n\n";

    TraceLogger::instance().initialize("lock_test");

    std::cout << "[Test 1] Counter Consistency Test\n";
    CounterConsistencyTest test1(8, 1000);
    test1.run_with_mutex();
    test1.run_with_spinlock();
    test1.run_with_atomic();
    std::cout << "\n";

    std::cout << "[Test 2] Deadlock Scenario Test\n";
    DeadlockScenarioTest test2;
    test2.run_correct_order();
    test2.run_wrong_order_with_timeout();
    std::cout << "\n";

    std::cout << "[Test 3] Sharded Lock Test\n";
    ShardedLockTest test3(16, 8, 500);
    test3.run();
    std::cout << "\n";

    TraceAnalyzer analyzer;
    analyzer.add_validator(std::make_shared<LockInvariantValidator>());

    auto events = TraceLogger::instance().get_events();
    auto lock_infos = TraceLogger::instance().get_lock_infos();
    auto memory_accesses = TraceLogger::instance().get_memory_accesses();

    AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);
    std::cout << report.to_string();

    std::string log_path = TraceLogger::instance().get_log_file_path();
    std::string report_path = log_path.substr(0, log_path.find_last_of('.')) + "_report.txt";
    report.save_to_file(report_path);
    std::cout << "\nReport saved to: " << report_path << "\n";

    TraceLogger::instance().flush_and_close();
}

void run_memory_pool_tests() {
    std::cout << "\n========================================\n";
    std::cout << "       MemoryPool Concurrency Tests\n";
    std::cout << "========================================\n\n";

    TraceLogger::instance().initialize("memory_pool_test");

    std::cout << "[Test 1] Memory Consistency Test\n";
    MemoryPoolConsistencyTest test1(4, 100);
    test1.run();
    std::cout << "\n";

    std::cout << "[Test 2] Memory Stress Test\n";
    MemoryPoolStressTest test2(4, 1000);
    test2.run();
    std::cout << "\n";

    std::cout << "[Test 3] Memory Boundary Test\n";
    MemoryBoundaryTest test3;
    test3.run();
    std::cout << "\n";

    TraceAnalyzer analyzer;
    analyzer.add_validator(std::make_shared<MemoryPoolInvariantValidator>());

    auto events = TraceLogger::instance().get_events();
    auto lock_infos = TraceLogger::instance().get_lock_infos();
    auto memory_accesses = TraceLogger::instance().get_memory_accesses();

    AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);
    std::cout << report.to_string();

    std::string log_path = TraceLogger::instance().get_log_file_path();
    std::string report_path = log_path.substr(0, log_path.find_last_of('.')) + "_report.txt";
    report.save_to_file(report_path);
    std::cout << "\nReport saved to: " << report_path << "\n";

    TraceLogger::instance().flush_and_close();
}

} // namespace testing
} // namespace cc_server
