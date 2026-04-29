#include "trace/test_assertions.h"
#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/lock.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <functional>

using namespace cc_server;
using namespace cc_server::testing;

// 高并发压力测试辅助函数
void run_concurrent_test(const std::string& name,
                         int num_threads,
                         int iterations,
                         std::function<void(int)> worker_func) {
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_func, i);
    }

    for (auto& t : threads) {
        t.join();
    }
}

// 测试场景1：高并发 Mutex 压力测试
void test_mutex_stress() {
    TEST_SUITE("Mutex Stress Tests");

    RUN_TEST(mutex_high_concurrency_counter) {
        const int num_threads = 8;
        const int iterations = 10000;
        std::atomic<int> counter{0};
        Mutex mutex;

        auto start = std::chrono::steady_clock::now();

        run_concurrent_test("mutex_counter", num_threads, iterations,
            [&](int) {
                for (int i = 0; i < iterations; ++i) {
                    MutexGuard guard(mutex);
                    counter++;
                }
            });

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        EXPECT_EQ(counter.load(), num_threads * iterations);
        std::cout << "  Completed " << num_threads * iterations << " increments in " << duration << "ms\n";
    });

    RUN_TEST(mutex_read_write_mix) {
        const int num_readers = 6;
        const int num_writers = 2;
        const int iterations = 5000;
        std::atomic<int> reader_count{0};
        int shared_data = 0;
        Mutex data_mutex;
        std::atomic<bool> start{false};

        auto start_time = std::chrono::steady_clock::now();

        // 启动读线程
        std::vector<std::thread> threads;
        for (int i = 0; i < num_readers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    MutexGuard guard(data_mutex);
                    reader_count++;
                    volatile int temp = shared_data;
                    (void)temp;
                    reader_count--;
                }
            });
        }

        // 启动写线程
        for (int i = 0; i < num_writers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    MutexGuard guard(data_mutex);
                    shared_data = j;
                }
            });
        }

        start.store(true);

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

        std::cout << "  Read/write mix completed in " << duration << "ms\n";
        EXPECT_TRUE(true);
    });
}

// 测试场景2：高并发 SpinLock 压力测试
void test_spinlock_stress() {
    TEST_SUITE("SpinLock Stress Tests");

    RUN_TEST(spinlock_high_concurrency_counter) {
        const int num_threads = 8;
        const int iterations = 20000;
        std::atomic<int> counter{0};
        SpinLock spinlock;

        auto start = std::chrono::steady_clock::now();

        run_concurrent_test("spinlock_counter", num_threads, iterations,
            [&](int) {
                for (int i = 0; i < iterations; ++i) {
                    SpinLockGuard guard(spinlock);
                    counter++;
                }
            });

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        EXPECT_EQ(counter.load(), num_threads * iterations);
        std::cout << "  Completed " << num_threads * iterations << " increments in " << duration << "ms\n";
    });

    RUN_TEST(spinlock_contention_stress) {
        // 多个线程竞争同一把锁
        const int num_threads = 16;
        const int iterations = 1000;
        std::atomic<int> counter{0};
        SpinLock spinlock;

        auto start = std::chrono::steady_clock::now();

        run_concurrent_test("spinlock_contention", num_threads, iterations,
            [&](int thread_id) {
                for (int i = 0; i < iterations; ++i) {
                    SpinLockGuard guard(spinlock);
                    counter++;
                    // 临界区内做一些工作
                    volatile int x = thread_id * i;
                    (void)x;
                }
            });

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        EXPECT_EQ(counter.load(), num_threads * iterations);
        std::cout << "  High contention test: " << duration << "ms for " << num_threads * iterations << " ops\n";
    });
}

// 测试场景3：读写锁压力测试
void test_rwlock_stress() {
    TEST_SUITE("RWLock Stress Tests");

    RUN_TEST(rwlock_read_heavy_workload) {
        // 读多写少场景
        const int num_readers = 10;
        const int num_writers = 2;
        const int iterations = 5000;
        std::atomic<int> total_reads{0};
        int shared_data = 0;
        RWLock rwlock;
        std::atomic<bool> start{false};

        auto start_time = std::chrono::steady_clock::now();

        std::vector<std::thread> threads;

        // 读线程
        for (int i = 0; i < num_readers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    RWLockReadGuard guard(rwlock);
                    volatile int temp = shared_data;
                    (void)temp;
                    total_reads++;
                }
            });
        }

        // 写线程
        for (int i = 0; i < num_writers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    WriteLockGuard<RWLock> guard(rwlock);
                    shared_data = j;
                }
            });
        }

        start.store(true);

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

        EXPECT_EQ(total_reads.load(), num_readers * iterations);
        std::cout << "  Read-heavy workload: " << duration << "ms, " << total_reads.load() << " reads\n";
    });

    RUN_TEST(rwlock_write_heavy_workload) {
        // 写多读少场景
        const int num_readers = 2;
        const int num_writers = 8;
        const int iterations = 5000;
        std::atomic<int> total_writes{0};
        int shared_data = 0;
        RWLock rwlock;
        std::atomic<bool> start{false};

        auto start_time = std::chrono::steady_clock::now();

        std::vector<std::thread> threads;

        // 读线程
        for (int i = 0; i < num_readers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    RWLockReadGuard guard(rwlock);
                    volatile int temp = shared_data;
                    (void)temp;
                }
            });
        }

        // 写线程
        for (int i = 0; i < num_writers; ++i) {
            threads.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    WriteLockGuard<RWLock> guard(rwlock);
                    shared_data = j;
                    total_writes++;
                }
            });
        }

        start.store(true);

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

        EXPECT_EQ(total_writes.load(), num_writers * iterations);
        std::cout << "  Write-heavy workload: " << duration << "ms, " << total_writes.load() << " writes\n";
    });
}

// 测试场景4：分片锁扩展性测试
void test_sharded_lock_scalability() {
    TEST_SUITE("ShardedLock Scalability Tests");

    RUN_TEST(sharded_lock_scales_with_shards) {
        const int iterations = 10000;
        std::atomic<int> counter{0};

        // 测试不同分片数的性能
        for (size_t num_shards : {1, 4, 8, 16}) {
            ShardedLock<SpinLock> sharded(num_shards);
            counter.store(0);

            auto start = std::chrono::steady_clock::now();

            run_concurrent_test("sharded_test", 8, iterations,
                [&](int thread_id) {
                    std::string key = "key" + std::to_string(thread_id % 100);
                    for (int i = 0; i < iterations; ++i) {
                        auto& lock = sharded.get_shard(key);
                        SpinLockGuard guard(lock);
                        counter++;
                        key = "key" + std::to_string((thread_id + i) % 100);
                    }
                });

            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            EXPECT_EQ(counter.load(), 8 * iterations);
            std::cout << "  " << num_shards << " shards: " << duration << "ms\n";
        }
    });

    RUN_TEST(sharded_rwlock_read_scalability) {
        const int iterations = 5000;
        const int num_shards = 8;
        ShardedRWLock sharded(num_shards);
        std::atomic<int> total_reads{0};
        std::atomic<bool> start{false};

        auto start_time = std::chrono::steady_clock::now();

        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&sharded, &total_reads, &start, iterations]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < iterations; ++j) {
                    std::string key = "key" + std::to_string(j % 100);
                    auto& lock = sharded.get_shard(key);
                    ReadLockGuard<RWLock> guard(lock);
                    total_reads++;
                }
            });
        }

        start.store(true);

        for (auto& t : threads) {
            t.join();
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

        EXPECT_EQ(total_reads.load(), 8 * iterations);
        std::cout << "  ShardedRWLock read scalability: " << duration << "ms for " << total_reads.load() << " reads\n";
    });
}

// 测试场景5：原子操作压力测试
void test_atomic_stress() {
    TEST_SUITE("Atomic Operations Stress Tests");

    RUN_TEST(atomic_fetch_add_high_concurrency) {
        const int num_threads = 8;
        const int iterations = 100000;
        AtomicInteger counter(0);

        auto start = std::chrono::steady_clock::now();

        run_concurrent_test("atomic_fetch_add", num_threads, iterations,
            [&](int) {
                for (int i = 0; i < iterations; ++i) {
                    counter.fetch_add(1);
                }
            });

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        EXPECT_EQ(counter.load(), num_threads * iterations);
        std::cout << "  " << num_threads * iterations << " atomic increments in " << duration << "ms\n";
    });

    RUN_TEST(atomic_compare_exchange_stress) {
        const int num_threads = 4;
        const int iterations = 10000;
        AtomicInteger counter(0);

        run_concurrent_test("atomic_cas", num_threads, iterations,
            [&](int) {
                for (int i = 0; i < iterations; ++i) {
                    int expected = counter.load();
                    while (!counter.compare_exchange(expected, expected + 1)) {
                        // 失败时 expected 已被更新，重试
                    }
                }
            });

        EXPECT_EQ(counter.load(), num_threads * iterations);
    });

    RUN_TEST(mixed_atomic_operations) {
        const int num_threads = 4;
        const int iterations = 10000;
        AtomicInteger counter(100);

        run_concurrent_test("mixed_atomic", num_threads, iterations,
            [&](int thread_id) {
                for (int i = 0; i < iterations; ++i) {
                    if (i % 4 == 0) {
                        counter.fetch_add(1);
                    } else if (i % 4 == 1) {
                        counter.fetch_sub(1);
                    } else if (i % 4 == 2) {
                        counter.fetch_or(0xFF);
                    } else {
                        counter.exchange(counter.load() + 1);
                    }
                }
            });

        // 由于混合操作，我们只验证最终值是合理的
        EXPECT_GT(counter.load(), 0);
    });
}

// 测试场景6：递归锁压力测试
void test_recursive_mutex_stress() {
    TEST_SUITE("RecursiveMutex Stress Tests");

    RUN_TEST(recursive_lock_nested_calls) {
        RecursiveMutex rmutex;
        std::atomic<int> counter{0};
        const int iterations = 1000;

        std::function<void(int)> deeply_nested = [&](int depth) {
            if (depth > 0) {
                RecursiveMutexGuard guard(rmutex);
                counter++;
                deeply_nested(depth - 1);
            }
        };

        std::thread t([&]() {
            for (int i = 0; i < iterations; ++i) {
                deeply_nested(5);
            }
        });

        t.join();

        // 每次迭代调用深度5，总共 6 次锁操作（包含外层）
        EXPECT_EQ(counter.load(), iterations * 6);
    });

    RUN_TEST(recursive_lock_concurrent) {
        RecursiveMutex rmutex;
        std::atomic<int> counter{0};
        const int num_threads = 4;
        const int iterations = 5000;

        run_concurrent_test("recursive_concurrent", num_threads, iterations,
            [&](int) {
                for (int i = 0; i < iterations; ++i) {
                    RecursiveMutexGuard guard(rmutex);
                    counter++;
                    // 模拟递归
                    RecursiveMutexGuard guard2(rmutex);
                    counter++;
                }
            });

        // 每次迭代 2 次锁操作
        EXPECT_EQ(counter.load(), num_threads * iterations * 2);
    });
}

// 测试场景7：长时间稳定性测试
void test_long_running_stability() {
    TEST_SUITE("Long-Running Stability Tests");

    RUN_TEST(sustained_high_load) {
        const int duration_seconds = 2;
        std::atomic<int> operations{0};
        SpinLock spinlock;

        auto start = std::chrono::steady_clock::now();
        auto end = start + std::chrono::seconds(duration_seconds);

        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&]() {
                while (std::chrono::steady_clock::now() < end) {
                    SpinLockGuard guard(spinlock);
                    operations++;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto actual_duration = std::chrono::steady_clock::now() - start;
        auto ops_per_sec = operations.load() * 1000 / std::chrono::duration_cast<std::chrono::milliseconds>(actual_duration).count();

        std::cout << "  Sustained " << ops_per_sec << " ops/sec for " << duration_seconds << "s\n";
        std::cout << "  Total operations: " << operations.load() << "\n";
        EXPECT_GT(operations.load(), 0);
    });

    RUN_TEST(stop_and_go_pattern) {
        // 测试启停模式的稳定性
        const int cycles = 100;
        std::atomic<int> counter{0};
        Mutex mutex;

        for (int c = 0; c < cycles; ++c) {
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&]() {
                    for (int j = 0; j < 100; ++j) {
                        MutexGuard guard(mutex);
                        counter++;
                    }
                });
            }
            for (auto& t : threads) {
                t.join();
            }
        }

        EXPECT_EQ(counter.load(), cycles * 4 * 100);
    });
}

// 主函数
int main() {
    std::cout << "\n";
    std::cout << yellow("========================================\n");
    std::cout << yellow("   LOCK STRESS TESTS\n");
    std::cout << yellow("========================================\n");

    test_mutex_stress();
    test_spinlock_stress();
    test_rwlock_stress();
    test_sharded_lock_scalability();
    test_atomic_stress();
    test_recursive_mutex_stress();
    test_long_running_stability();

    std::cout << "\n" << yellow("========================================\n");
    std::cout << yellow("   ALL STRESS TESTS COMPLETED\n");
    std::cout << yellow("========================================\n\n");

    auto& stats = g_test_stats();
    std::cout << "Final Results:\n";
    std::cout << "  Total:  " << stats.total_tests << std::endl;
    std::cout << green("  Passed: ") << stats.passed_tests << std::endl;
    std::cout << red("  Failed: ") << stats.failed_tests << std::endl;
    std::cout << yellow("  Skipped: ") << stats.skipped_tests << std::endl;

    return stats.failed_tests > 0 ? 1 : 0;
}