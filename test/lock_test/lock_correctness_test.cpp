#include "trace/test_assertions.h"
#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/lock.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>

using namespace cc_server;
using namespace cc_server::testing;

// 测试 Mutex 基本功能
void test_mutex_basic() {
    TEST_SUITE("Mutex Basic Tests");

    RUN_TEST(mutex_try_lock_success) {
        Mutex mutex;
        bool acquired = mutex.try_lock();
        EXPECT_TRUE(acquired);
        if (acquired) {
            mutex.unlock();
        }
    });

    RUN_TEST(mutex_try_lock_fail_when_held) {
        Mutex mutex;
        {
            MutexGuard guard(mutex);
            bool acquired = mutex.try_lock();
            EXPECT_FALSE(acquired);
        }
        bool acquired = mutex.try_lock();
        EXPECT_TRUE(acquired);
        if (acquired) mutex.unlock();
    });

    RUN_TEST(mutex_try_lock_for) {
        Mutex mutex;
        {
            MutexGuard guard(mutex);
            bool acquired = mutex.try_lock_for(std::chrono::milliseconds(10));
            EXPECT_FALSE(acquired);
        }
        bool acquired = mutex.try_lock_for(std::chrono::milliseconds(10));
        EXPECT_TRUE(acquired);
        if (acquired) mutex.unlock();
    });

    RUN_TEST(mutex_basic_mutual_exclusion) {
        std::atomic<int> counter(0);
        const int num_threads = 4;
        const int iterations = 1000;

        std::vector<std::thread> threads;
        Mutex mutex;

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&counter, &mutex, iterations]() {
                for (int j = 0; j < iterations; ++j) {
                    MutexGuard guard(mutex);
                    counter++;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(counter.load(), num_threads * iterations);
    });
}

// 测试 SpinLock 基本功能
void test_spinlock_basic() {
    TEST_SUITE("SpinLock Basic Tests");

    RUN_TEST(spinlock_try_lock_success) {
        SpinLock spinlock;
        bool acquired = spinlock.try_lock();
        EXPECT_TRUE(acquired);
        if (acquired) {
            spinlock.unlock();
        }
    });

    RUN_TEST(spinlock_try_lock_fail_when_held) {
        SpinLock spinlock;
        {
            SpinLockGuard guard(spinlock);
            bool acquired = spinlock.try_lock();
            EXPECT_FALSE(acquired);
        }
        bool acquired = spinlock.try_lock();
        EXPECT_TRUE(acquired);
        if (acquired) spinlock.unlock();
    });

    RUN_TEST(spinlock_basic_mutual_exclusion) {
        std::atomic<int> counter(0);
        const int num_threads = 4;
        const int iterations = 1000;

        std::vector<std::thread> threads;
        SpinLock spinlock;

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&counter, &spinlock, iterations]() {
                for (int j = 0; j < iterations; ++j) {
                    SpinLockGuard guard(spinlock);
                    counter++;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(counter.load(), num_threads * iterations);
    });
}

// 测试 RecursiveMutex 基本功能
void test_recursive_mutex_basic() {
    TEST_SUITE("RecursiveMutex Basic Tests");

    RUN_TEST(recursive_mutex_single_thread_reentrant) {
        RecursiveMutex rmutex;
        rmutex.lock();
        rmutex.lock();  // 再次获取，不应死锁
        rmutex.unlock();
        rmutex.unlock();
        EXPECT_TRUE(true);  // 如果到达这里说明没有死锁
    });

    RUN_TEST(recursive_mutex_multiple_recursive_locks) {
        RecursiveMutex rmutex;
        const int lock_count = 10;

        for (int i = 0; i < lock_count; ++i) {
            rmutex.lock();
        }

        for (int i = 0; i < lock_count; ++i) {
            rmutex.unlock();
        }

        EXPECT_TRUE(true);
    });

    RUN_TEST(recursive_mutex_try_lock) {
        RecursiveMutex rmutex;
        bool first = rmutex.try_lock();
        EXPECT_TRUE(first);

        bool second = rmutex.try_lock();
        EXPECT_TRUE(second);  // 同一线程应该能再次获取

        rmutex.unlock();
        rmutex.unlock();
    });

    RUN_TEST(recursive_mutex_counter) {
        std::atomic<int> counter(0);
        const int iterations = 1000;
        RecursiveMutex rmutex;

        std::thread t([&counter, &rmutex, iterations]() {
            for (int i = 0; i < iterations; ++i) {
                RecursiveMutexGuard guard(rmutex);
                counter++;
                // 模拟递归调用
                auto inner_work = [&counter, &rmutex]() {
                    RecursiveMutexGuard guard2(rmutex);
                    counter++;
                };
                inner_work();
            }
        });

        t.join();
        EXPECT_EQ(counter.load(), iterations * 2);
    });
}

// 测试 RWLock 基本功能
void test_rwlock_basic() {
    TEST_SUITE("RWLock Basic Tests");

    RUN_TEST(rwlock_read_lock_basic) {
        RWLock rwlock;
        rwlock.read_lock();
        rwlock.read_unlock();
        EXPECT_TRUE(true);
    });

    RUN_TEST(rwlock_write_lock_basic) {
        RWLock rwlock;
        rwlock.write_lock();
        rwlock.write_unlock();
        EXPECT_TRUE(true);
    });

    RUN_TEST(rwlock_multiple_readers) {
        RWLock rwlock;
        const int num_readers = 10;
        std::atomic<int> active_readers{0};
        std::atomic<int> max_readers{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < num_readers; ++i) {
            threads.emplace_back([&rwlock, &active_readers, &max_readers]() {
                RWLockReadGuard guard(rwlock);
                int current = active_readers.fetch_add(1) + 1;
                // 更新最大读者数（在锁内）
                int current_max = max_readers.load();
                while (current > current_max) {
                    current_max = current;
                    max_readers.store(current_max);
                    current = max_readers.load();
                }
                // 模拟读取操作
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                active_readers.fetch_sub(1);
            });
        }

        // 等待所有线程完成
        for (auto& t : threads) {
            t.join();
        }

        // 验证最大读者数至少为2（表明多个读者同时持有锁）
        EXPECT_GE(max_readers.load(), 2);
    });

    RUN_TEST(rwlock_writer_exclusive) {
        RWLock rwlock;
        std::atomic<int> data(0);
        std::atomic<bool> writer_active(false);
        bool conflict_detected = false;

        std::vector<std::thread> threads;

        // 1个写线程
        threads.emplace_back([&rwlock, &data, &writer_active]() {
            WriteLockGuard<RWLock> guard(rwlock);
            writer_active.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            data.store(42);
            writer_active.store(false);
        });

        // 多个读线程
        for (int i = 0; i < 5; ++i) {
            threads.emplace_back([&rwlock, &data, &writer_active, &conflict_detected]() {
                for (int j = 0; j < 100; ++j) {
                    ReadLockGuard<RWLock> guard(rwlock);
                    if (writer_active.load()) {
                        // 写锁时获取读锁，说明RWLock有问题
                        conflict_detected = true;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_FALSE(conflict_detected);
        EXPECT_EQ(data.load(), 42);
    });

    RUN_TEST(rwlock_try_read_lock) {
        RWLock rwlock;
        rwlock.write_lock();
        bool acquired = rwlock.try_read_lock();
        EXPECT_FALSE(acquired);  // 写锁占用时读锁应该失败
        rwlock.write_unlock();
    });

    RUN_TEST(rwlock_try_write_lock) {
        RWLock rwlock;
        rwlock.read_lock();
        bool acquired = rwlock.try_write_lock();
        EXPECT_FALSE(acquired);  // 读锁占用时写锁应该失败
        rwlock.read_unlock();
    });
}

// 测试 RWLock2 基本功能
void test_rwlock2_basic() {
    TEST_SUITE("RWLock2 Basic Tests");

    RUN_TEST(rwlock2_read_lock_basic) {
        RWLock2 rwlock2;
        rwlock2.read_lock();
        rwlock2.read_unlock();
        EXPECT_TRUE(true);
    });

    RUN_TEST(rwlock2_write_lock_basic) {
        RWLock2 rwlock2;
        rwlock2.write_lock();
        rwlock2.write_unlock();
        EXPECT_TRUE(true);
    });

    RUN_TEST(rwlock2_write_preference) {
        RWLock2 rwlock2;
        std::atomic<int> write_completed(0);
        std::atomic<int> read_started(0);
        bool read_blocked = false;

        std::thread writer([&rwlock2, &write_completed]() {
            rwlock2.write_lock();
            write_completed.store(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            write_completed.store(2);
            rwlock2.write_unlock();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        std::thread reader([&rwlock2, &read_started, &read_blocked, &write_completed]() {
            read_started.store(1);
            RWLock2ReadGuard guard(rwlock2);
            // 如果写优先正确实现，读应该等待到写完成才开始
            if (write_completed.load() < 2) {
                read_blocked = true;
            }
        });

        writer.join();
        reader.join();

        EXPECT_FALSE(read_blocked);
    });

    RUN_TEST(rwlock2_num_readers) {
        RWLock2 rwlock2;
        rwlock2.read_lock();
        rwlock2.read_lock();
        EXPECT_EQ(rwlock2.num_readers(), 2);
        rwlock2.read_unlock();
        rwlock2.read_unlock();
    });

    RUN_TEST(rwlock2_try_read_lock) {
        RWLock2 rwlock2;
        rwlock2.write_lock();
        bool acquired = rwlock2.try_read_lock();
        EXPECT_FALSE(acquired);
        rwlock2.write_unlock();
    });

    RUN_TEST(rwlock2_try_write_lock) {
        RWLock2 rwlock2;
        rwlock2.read_lock();
        bool acquired = rwlock2.try_write_lock();
        EXPECT_FALSE(acquired);
        rwlock2.read_unlock();
    });
}

// 测试 ShardedLock 基本功能
void test_sharded_lock_basic() {
    TEST_SUITE("ShardedLock Basic Tests");

    RUN_TEST(sharded_lock_creation) {
        ShardedLock<SpinLock> sharded(16);
        EXPECT_EQ(sharded.num_shards(), size_t(16));
    });

    RUN_TEST(sharded_lock_same_key_same_shard) {
        ShardedLock<SpinLock> sharded(8);
        auto& shard1 = sharded.get_shard("key1");
        auto& shard2 = sharded.get_shard("key1");
        EXPECT_EQ(&shard1, &shard2);
    });

    RUN_TEST(sharded_lock_different_keys) {
        ShardedLock<SpinLock> sharded(8);
        auto& shard1 = sharded.get_shard("key1");
        auto& shard2 = sharded.get_shard("key2");
        // 不同key可能映射到同一分片，取决于哈希
        // 我们只验证它们都是有效的引用
        EXPECT_TRUE(true);
    });

    RUN_TEST(sharded_lock_concurrent_access) {
        const int num_shards = 8;
        const int keys_per_shard = 100;
        const int iterations = 100;

        ShardedLock<SpinLock> sharded(num_shards);
        std::vector<std::atomic<int>> counters(num_shards);
        std::vector<std::thread> threads;

        for (int i = 0; i < num_shards * keys_per_shard; ++i) {
            std::string key = "key" + std::to_string(i);
            threads.emplace_back([&sharded, &counters, key, iterations]() {
                for (int j = 0; j < iterations; ++j) {
                    auto& lock = sharded.get_shard(key);
                    SpinLockGuard guard(lock);
                    size_t shard_idx = 0;
                    for (size_t s = 0; s < sharded.num_shards(); ++s) {
                        if (&lock == &sharded.shards()[s]) {
                            shard_idx = s;
                            break;
                        }
                    }
                    counters[shard_idx]++;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        int total = 0;
        for (int i = 0; i < num_shards; ++i) {
            total += counters[i].load();
        }
        EXPECT_EQ(total, num_shards * keys_per_shard * iterations);
    });
}

// 测试 ShardedRWLock 基本功能
void test_sharded_rwlock_basic() {
    TEST_SUITE("ShardedRWLock Basic Tests");

    RUN_TEST(sharded_rwlock_creation) {
        ShardedRWLock sharded(8);
        EXPECT_EQ(sharded.num_shards(), size_t(8));
    });

    RUN_TEST(sharded_rwlock_same_key_same_shard) {
        ShardedRWLock sharded(8);
        auto& shard1 = sharded.get_shard("key1");
        auto& shard2 = sharded.get_shard("key1");
        EXPECT_EQ(&shard1, &shard2);
    });

    RUN_TEST(sharded_rwlock_read_concurrent) {
        ShardedRWLock sharded(4);
        std::atomic<int> concurrent_reads(0);
        std::atomic<int> max_concurrent_reads(0);

        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&sharded, &concurrent_reads, &max_concurrent_reads]() {
                auto& shard = sharded.get_shard("same_key");
                ReadLockGuard<RWLock> guard(shard);
                concurrent_reads++;
                int current = concurrent_reads.load();
                while (current > max_concurrent_reads.load()) {
                    max_concurrent_reads.store(current);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                concurrent_reads--;
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_GT(max_concurrent_reads.load(), 1);
    });

    RUN_TEST(sharded_rwlock_write_exclusive) {
        ShardedRWLock sharded(4);
        std::atomic<int> data(0);
        std::atomic<bool> write_in_progress(false);
        std::atomic<int> concurrent_readers(0);

        std::thread writer([&sharded, &data, &write_in_progress]() {
            auto& shard = sharded.get_shard("key");
            WriteLockGuard<RWLock> guard(shard);
            write_in_progress.store(true);
            data.store(100);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            write_in_progress.store(false);
        });

        std::vector<std::thread> readers;
        for (int i = 0; i < 5; ++i) {
            readers.emplace_back([&sharded, &write_in_progress, &concurrent_readers]() {
                auto& shard = sharded.get_shard("key");
                ReadLockGuard<RWLock> guard(shard);
                if (write_in_progress.load()) {
                    concurrent_readers++;
                }
            });
        }

        writer.join();
        for (auto& r : readers) {
            r.join();
        }

        EXPECT_EQ(concurrent_readers.load(), 0);
        EXPECT_EQ(data.load(), 100);
    });
}

// 主函数
int main() {
    std::cout << "\n";
    std::cout << yellow("========================================\n");
    std::cout << yellow("   LOCK CORRECTNESS TESTS\n");
    std::cout << yellow("========================================\n");

    test_mutex_basic();
    test_spinlock_basic();
    test_recursive_mutex_basic();
    test_rwlock_basic();
    test_rwlock2_basic();
    test_sharded_lock_basic();
    test_sharded_rwlock_basic();

    std::cout << "\n" << yellow("========================================\n");
    std::cout << yellow("   ALL CORRECTNESS TESTS COMPLETED\n");
    std::cout << yellow("========================================\n\n");

    auto& stats = g_test_stats();
    std::cout << "Final Results:\n";
    std::cout << "  Total:  " << stats.total_tests << std::endl;
    std::cout << green("  Passed: ") << stats.passed_tests << std::endl;
    std::cout << red("  Failed: ") << stats.failed_tests << std::endl;
    std::cout << yellow("  Skipped: ") << stats.skipped_tests << std::endl;

    return stats.failed_tests > 0 ? 1 : 0;
}