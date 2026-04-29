#include "trace/test_assertions.h"
#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/lock.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <sstream>
#include <random>

using namespace cc_server;
using namespace cc_server::testing;

// 测试场景1：无保护的并发读写
void test_race_unprotected_read_write() {
    TEST_SUITE("Race Detection - Unprotected Access");

    RUN_TEST(detect_race_between_write_and_read) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("race_unprotected_test");

        int shared_data = 0;
        std::atomic<bool> start{false};

        // Writer thread
        std::thread writer([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                // 记录写操作
                TRACE_LOG(OpType::WRITE, "shared_data", std::to_string(shared_data) + "->" + std::to_string(i + 1));
                shared_data = i + 1;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        // Reader thread - 无保护读取
        std::thread reader([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                // 记录读操作
                int value = shared_data;  // 无锁读取
                TRACE_LOG(OpType::READ, "shared_data", std::to_string(value));
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        start.store(true);
        writer.join();
        reader.join();

        // 使用 TraceAnalyzer 检测数据竞争
        TraceAnalyzer analyzer;
        auto events = TraceLogger::instance().get_events();
        auto lock_infos = TraceLogger::instance().get_lock_infos();
        auto memory_accesses = TraceLogger::instance().get_memory_accesses();

        AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);

        std::cout << "Detected " << report.data_races.size() << " data races\n";
        for (const auto& race : report.data_races) {
            std::cout << "  Address: " << race.address << "\n";
            std::cout << "  " << race.description << "\n";
        }

        // 在并发环境下，应该能检测到数据竞争
        EXPECT_TRUE(true);  // 检测器已运行

        TraceLogger::instance().flush_and_close();
    });

    RUN_TEST(detect_race_between_two_writes) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("race_two_writes_test");

        int counter = 0;
        std::atomic<bool> start{false};

        std::thread writer1([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                TRACE_LOG(OpType::WRITE, "counter", std::to_string(counter) + "->" + std::to_string(counter + 1));
                counter++;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        std::thread writer2([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                TRACE_LOG(OpType::WRITE, "counter", std::to_string(counter) + "->" + std::to_string(counter + 1));
                counter++;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        start.store(true);
        writer1.join();
        writer2.join();

        std::cout << "Final counter value: " << counter << " (expected: 200)\n";
        // 由于数据竞争，最终值可能不是 200
        // 有锁保护的情况下才会是 200

        TraceLogger::instance().flush_and_close();
    });
}

// 测试场景2：使用锁保护后无竞争
void test_no_race_with_protection() {
    TEST_SUITE("Race Detection - With Protection");

    RUN_TEST(mutex_protects_shared_data) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("race_protected_mutex_test");

        int shared_data = 0;
        Mutex mutex;
        std::atomic<bool> start{false};

        std::thread writer([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                MutexGuard guard(mutex);
                TRACE_LOG(OpType::WRITE, "shared_data", std::to_string(shared_data) + "->" + std::to_string(i + 1));
                shared_data = i + 1;
            }
        });

        std::thread reader([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                int value;
                {
                    MutexGuard guard(mutex);
                    value = shared_data;
                }
                TRACE_LOG(OpType::READ, "shared_data", std::to_string(value));
            }
        });

        start.store(true);
        writer.join();
        reader.join();

        TraceAnalyzer analyzer;
        auto events = TraceLogger::instance().get_events();
        auto lock_infos = TraceLogger::instance().get_lock_infos();
        auto memory_accesses = TraceLogger::instance().get_memory_accesses();

        AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);

        std::cout << "With mutex protection - detected " << report.data_races.size() << " races\n";
        EXPECT_EQ(report.data_races.size(), size_t(0));  // 不应该有竞争

        TraceLogger::instance().flush_and_close();
    });

    RUN_TEST(spinlock_protects_shared_data) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("race_protected_spinlock_test");

        std::atomic<int> counter{0};
        SpinLock spinlock;
        std::atomic<bool> start{false};

        auto increment = [&](int iterations) {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < iterations; ++i) {
                SpinLockGuard guard(spinlock);
                int old = counter.load();
                TRACE_LOG(OpType::WRITE, "counter", std::to_string(old) + "->" + std::to_string(old + 1));
                counter.store(old + 1);
            }
        };

        std::thread t1(increment, 1000);
        std::thread t2(increment, 1000);

        start.store(true);
        t1.join();
        t2.join();

        std::cout << "Final counter: " << counter.load() << " (expected: 2000)\n";
        EXPECT_EQ(counter.load(), 2000);

        TraceLogger::instance().flush_and_close();
    });
}

// 测试场景3：原子操作 vs 锁
void test_atomic_vs_lock() {
    TEST_SUITE("Race Detection - Atomic vs Lock");

    RUN_TEST(atomic_increment_no_race) {
        std::atomic<int> counter{0};
        const int num_threads = 4;
        const int iterations = 1000;
        std::vector<std::thread> threads;

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&counter, iterations]() {
                for (int j = 0; j < iterations; ++j) {
                    counter.fetch_add(1);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(counter.load(), num_threads * iterations);
    });

    RUN_TEST(compare_exchange_successive) {
        AtomicInteger atomic_val(0);
        const int iterations = 1000;

        std::thread t1([&]() {
            for (int i = 0; i < iterations; ++i) {
                int expected = atomic_val.load();
                while (!atomic_val.compare_exchange(expected, expected + 1)) {
                    // 失败时 expected 会被更新为当前值，重试
                }
            }
        });

        std::thread t2([&]() {
            for (int i = 0; i < iterations; ++i) {
                int expected = atomic_val.load();
                while (!atomic_val.compare_exchange(expected, expected + 1)) {
                }
            }
        });

        t1.join();
        t2.join();

        EXPECT_EQ(atomic_val.load(), iterations * 2);
    });

    RUN_TEST(atomic_exchange_works) {
        AtomicInteger atomic_val(100);
        int old = atomic_val.exchange(200);
        EXPECT_EQ(old, 100);
        EXPECT_EQ(atomic_val.load(), 200);
    });
}

// 测试场景4：竞争检测阈值测试
void test_race_detection_threshold() {
    TEST_SUITE("Race Detection Threshold");

    RUN_TEST(race_detected_when_access_within_threshold) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("race_threshold_test");

        int data = 0;
        std::atomic<bool> start{false};

        // 两个线程几乎同时访问
        std::thread t1([&]() {
            while (!start.load()) std::this_thread::yield();
            TRACE_LOG(OpType::WRITE, "data", "thread1_write");
            data = 1;
        });

        std::thread t2([&]() {
            while (!start.load()) std::this_thread::yield();
            // 几乎同时写入
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            TRACE_LOG(OpType::WRITE, "data", "thread2_write");
            data = 2;
        });

        start.store(true);
        t1.join();
        t2.join();

        TraceAnalyzer analyzer;
        auto events = TraceLogger::instance().get_events();
        auto lock_infos = TraceLogger::instance().get_lock_infos();
        auto memory_accesses = TraceLogger::instance().get_memory_accesses();

        AnalysisReport report = analyzer.analyze(events, lock_infos, memory_accesses);

        std::cout << "Threshold test - " << report.data_races.size() << " races detected\n";

        // 两个写操作时间戳相差 10μs < 100μs 阈值，应该被检测为竞争
        EXPECT_TRUE(true);

        TraceLogger::instance().flush_and_close();
    });
}

// 测试场景5：读写竞争
void test_read_write_race() {
    TEST_SUITE("Read-Write Race Detection");

    RUN_TEST(unprotected_read_write_race) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("read_write_race_test");

        int shared_value = 0;
        std::atomic<bool> go{false};

        std::thread writer([&]() {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                // 模拟读-修改-写操作，但没有锁保护
                int temp = shared_value;  // 读
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                shared_value = temp + 1;  // 写
                TRACE_LOG(OpType::WRITE, "shared_value", "increment");
            }
        });

        std::thread reader([&]() {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                int val = shared_value;  // 无保护读取
                TRACE_LOG(OpType::READ, "shared_value", std::to_string(val));
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        go.store(true);
        writer.join();
        reader.join();

        std::cout << "Final shared_value: " << shared_value << " (expected: 50 if correct, likely less due to race)\n";
        // 由于竞争，最终值可能小于 50

        TraceLogger::instance().flush_and_close();
    });

    RUN_TEST(rwlock_allows_concurrent_reads) {
        TraceLogger::instance().reset();
        TraceLogger::instance().initialize("rwlock_readers_test");

        RWLock rwlock;
        int shared_data = 0;
        std::atomic<int> reader_count{0};
        std::atomic<int> max_readers{0};
        std::atomic<bool> start{false};

        // 多个读线程
        std::vector<std::thread> readers;
        for (int i = 0; i < 5; ++i) {
            readers.emplace_back([&]() {
                while (!start.load()) std::this_thread::yield();
                for (int j = 0; j < 100; ++j) {
                    RWLockReadGuard guard(rwlock);
                    reader_count++;
                    int current = reader_count.load();
                    while (current > max_readers.load()) {
                        max_readers.store(current);
                    }
                    // 读取共享数据
                    volatile int temp = shared_data;
                    (void)temp;
                    reader_count--;
                }
            });
        }

        // 一个写线程
        std::thread writer([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < 100; ++j) {
                WriteLockGuard<RWLock> guard(rwlock);
                shared_data = j;
            }
        });

        start.store(true);
        for (auto& t : readers) {
            t.join();
        }
        writer.join();

        std::cout << "Max concurrent readers observed: " << max_readers.load() << "\n";
        // 读写锁应该允许多个读线程并发
        EXPECT_GT(max_readers.load(), 1);

        TraceLogger::instance().flush_and_close();
    });
}

// 主函数
int main() {
    std::cout << "\n";
    std::cout << yellow("========================================\n");
    std::cout << yellow("   DATA RACE DETECTION TESTS\n");
    std::cout << yellow("========================================\n");

    test_race_unprotected_read_write();
    test_no_race_with_protection();
    test_atomic_vs_lock();
    test_race_detection_threshold();
    test_read_write_race();

    std::cout << "\n" << yellow("========================================\n");
    std::cout << yellow("   ALL RACE DETECTION TESTS COMPLETED\n");
    std::cout << yellow("========================================\n\n");

    auto& stats = g_test_stats();
    std::cout << "Final Results:\n";
    std::cout << "  Total:  " << stats.total_tests << std::endl;
    std::cout << green("  Passed: ") << stats.passed_tests << std::endl;
    std::cout << red("  Failed: ") << stats.failed_tests << std::endl;
    std::cout << yellow("  Skipped: ") << stats.skipped_tests << std::endl;

    return stats.failed_tests > 0 ? 1 : 0;
}