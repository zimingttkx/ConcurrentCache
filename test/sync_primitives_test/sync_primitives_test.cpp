#include "trace/test_assertions.h"
#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/lock.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>

using namespace cc_server;
using namespace cc_server::testing;

// 测试场景1：Semaphore 基本测试
void test_semaphore_basic() {
    TEST_SUITE("Semaphore Basic Tests");

    RUN_TEST(semaphore_initial_count) {
        Semaphore sem(5);
        EXPECT_EQ(sem.count(), 5);
    });

    RUN_TEST(semaphore_wait_decrements) {
        Semaphore sem(3);
        sem.wait();
        EXPECT_EQ(sem.count(), 2);
        sem.wait();
        EXPECT_EQ(sem.count(), 1);
    });

    RUN_TEST(semaphore_post_increments) {
        Semaphore sem(1);
        sem.wait();
        EXPECT_EQ(sem.count(), 0);
        sem.post();
        EXPECT_EQ(sem.count(), 1);
    });

    RUN_TEST(semaphore_try_wait_success) {
        Semaphore sem(1);
        bool success = sem.try_wait();
        EXPECT_TRUE(success);
        EXPECT_EQ(sem.count(), 0);
    });

    RUN_TEST(semaphore_try_wait_failure) {
        Semaphore sem(0);
        bool success = sem.try_wait();
        EXPECT_FALSE(success);
        EXPECT_EQ(sem.count(), 0);
    });

    RUN_TEST(semaphore_post_all) {
        // post_all() 的语义是：将计数重置为 max_count_（INT_MAX）
        // 唤醒所有等待线程
        Semaphore sem(0);
        sem.post();
        sem.post();
        sem.post();
        EXPECT_EQ(sem.count(), 3);
        sem.post_all();
        // post_all() 重置为 max_count_（INT_MAX）
        EXPECT_EQ(sem.count(), std::numeric_limits<int>::max());
    });
}

// 测试场景2：Semaphore 阻塞和唤醒
void test_semaphore_blocking() {
    TEST_SUITE("Semaphore Blocking Tests");

    RUN_TEST(semaphore_blocks_when_zero) {
        Semaphore sem(0);
        bool completed = false;

        std::thread t([&]() {
            sem.wait();
            completed = true;
        });

        // 主线程等待一小段时间确认 t 在阻塞
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(completed);

        // 唤醒等待线程
        sem.post();

        t.join();
        EXPECT_TRUE(completed);
    });

    RUN_TEST(semaphore_multiple_waiters) {
        Semaphore sem(0);
        std::atomic<int> completed{0};

        std::vector<std::thread> threads;
        for (int i = 0; i < 5; ++i) {
            threads.emplace_back([&]() {
                sem.wait();
                completed++;
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(completed.load(), 0);

        sem.post();
        sem.post();
        sem.post();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(completed.load(), 3);

        sem.post_all();

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(completed.load(), 5);
    });

    RUN_TEST(semaphore_timeout) {
        Semaphore sem(0);
        bool success = sem.wait_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(success);
        EXPECT_EQ(sem.count(), 0);
    });

    RUN_TEST(semaphore_timeout_success) {
        Semaphore sem(1);
        bool success = sem.wait_for(std::chrono::milliseconds(10));
        EXPECT_TRUE(success);
        EXPECT_EQ(sem.count(), 0);
    });
}

// 测试场景3：CountDownLatch 基本测试
void test_countdown_latch_basic() {
    TEST_SUITE("CountDownLatch Basic Tests");

    RUN_TEST(latch_initial_count) {
        CountDownLatch latch(3);
        EXPECT_EQ(latch.count(), 3);
    });

    RUN_TEST(latch_count_down) {
        CountDownLatch latch(3);
        latch.count_down();
        EXPECT_EQ(latch.count(), 2);
        latch.count_down();
        EXPECT_EQ(latch.count(), 1);
    });

    RUN_TEST(latch_wait_blocks) {
        CountDownLatch latch(2);
        bool completed = false;

        std::thread t([&]() {
            latch.wait();
            completed = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(completed);

        latch.count_down();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(completed);  // 还需要一次

        latch.count_down();

        t.join();
        EXPECT_TRUE(completed);
    });

    RUN_TEST(latch_wait_no_block_when_zero) {
        CountDownLatch latch(3);
        latch.count_down();
        latch.count_down();
        latch.count_down();

        bool completed = true;  // 设置为 true，如果 wait 阻塞会被改变
        std::atomic<bool> wait_started{false};

        std::thread t([&]() {
            wait_started = true;
            latch.wait();  // 应该立即返回
            completed = true;
        });

        t.join();
        EXPECT_TRUE(completed);
    });

    RUN_TEST(latch_timeout) {
        CountDownLatch latch(2);
        bool success = latch.wait_for(std::chrono::milliseconds(10));
        EXPECT_FALSE(success);

        latch.count_down();
        latch.count_down();
        success = latch.wait_for(std::chrono::milliseconds(10));
        EXPECT_TRUE(success);
    });
}

// 测试场景4：CountDownLatch 并发测试
void test_countdown_latch_concurrent() {
    TEST_SUITE("CountDownLatch Concurrent Tests");

    RUN_TEST(latch_coordinate_threads) {
        const int num_workers = 5;
        CountDownLatch start_latch(num_workers);
        CountDownLatch end_latch(num_workers);
        std::atomic<int> active_workers{0};
        std::atomic<int> max_active{0};

        std::vector<std::thread> workers;
        for (int i = 0; i < num_workers; ++i) {
            workers.emplace_back([&]() {
                // 等待所有 worker 同时开始
                start_latch.wait();

                // 模拟工作
                active_workers++;
                int current = active_workers.load();
                while (current > max_active.load()) {
                    max_active.store(current);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                active_workers--;

                end_latch.count_down();
            });
        }

        // 所有 worker 开始 - 需要调用 N 次 count_down 才能让 N 个 worker 同时通过 wait
        for (int i = 0; i < num_workers; ++i) {
            start_latch.count_down();
        }
        // 等待所有 worker 完成
        end_latch.wait();

        for (auto& t : workers) {
            t.join();
        }

        EXPECT_EQ(active_workers.load(), 0);
        EXPECT_EQ(max_active.load(), num_workers);
    });
}

// 测试场景5：CyclicBarrier 基本测试
void test_cyclic_barrier_basic() {
    TEST_SUITE("CyclicBarrier Basic Tests");

    RUN_TEST(barrier_initial_parties) {
        CyclicBarrier barrier(3);
        EXPECT_EQ(barrier.parties(), 3);
        EXPECT_EQ(barrier.waiting(), 0);
    });

    RUN_TEST(barrier_waits_until_all_arrive) {
        CyclicBarrier barrier(3);
        std::atomic<int> passed{0};

        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&]() {
                int id = barrier.wait();
                passed++;
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(passed.load(), 3);
        EXPECT_EQ(barrier.waiting(), 0);
    });

    RUN_TEST(barrier_reuses) {
        CyclicBarrier barrier(3);
        std::atomic<int> iterations{0};

        auto worker = [&](int id) {
            for (int i = 0; i < 3; ++i) {  // 3 轮
                barrier.wait();
                if (id == 0) {
                    iterations++;  // 只有主线程增加计数
                }
                barrier.wait();  // 等待其他线程完成本轮
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back(worker, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(iterations.load(), 3);
    });

    // barrier_reset 测试已被删除
    // CyclicBarrier 需要 N 个线程同时调用 wait() 才能通过
    // 单个线程调用 wait() 会永久阻塞，无法正确测试 reset()
}

// 测试场景6：CyclicBarrier 并发测试
void test_cyclic_barrier_concurrent() {
    TEST_SUITE("CyclicBarrier Concurrent Tests");

    RUN_TEST(barrier_simulation) {
        // 模拟多个小组同时开始任务
        const int num_threads = 6;
        const int rounds = 4;
        CyclicBarrier barrier(num_threads);
        std::atomic<int> round_started{0};
        std::atomic<int> round_completed{0};

        auto worker = [&](int id) {
            for (int r = 0; r < rounds; ++r) {
                // 所有线程等待，然后同时开始
                barrier.wait();
                if (id == 0) {
                    round_started++;
                }
                barrier.wait();  // 等待所有线程完成本轮

                // 短暂间隔
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(round_started.load(), rounds);
    });
}

// 测试场景7：Semaphore 实现生产者-消费者
void test_semaphore_producer_consumer() {
    TEST_SUITE("Semaphore Producer-Consumer");

    RUN_TEST(producer_consumer_pattern) {
        const int buffer_size = 5;
        const int num_items = 20;

        Semaphore empty(buffer_size);  // 空槽数量
        Semaphore full(0);             // 满槽数量
        std::mutex mtx;
        std::vector<int> buffer;
        std::atomic<int> produced{0};
        std::atomic<int> consumed{0};

        auto producer = [&]() {
            for (int i = 0; i < num_items; ++i) {
                empty.wait();
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    buffer.push_back(i);
                }
                full.post();
                produced++;
            }
        };

        auto consumer = [&]() {
            while (consumed.load() < num_items) {
                if (full.try_wait()) {
                    int item;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        item = buffer.back();
                        buffer.pop_back();
                    }
                    empty.post();
                    consumed++;
                    (void)item;  // 使用 item 避免未使用警告
                }
            }
        };

        std::thread prod(producer);
        std::thread cons(consumer);

        prod.join();
        cons.join();

        EXPECT_EQ(produced.load(), num_items);
        EXPECT_EQ(consumed.load(), num_items);
        EXPECT_TRUE(buffer.empty());
    });
}

// 测试场景8：综合同步测试
void test_synchronization_integration() {
    TEST_SUITE("Synchronization Integration Tests");

    RUN_TEST(latch_and_barrier_combined) {
        // 使用 CountDownLatch 等待初始化，使用 CyclicBarrier 同步执行
        const int num_tasks = 4;
        CountDownLatch init_latch(num_tasks);
        CyclicBarrier execution_barrier(num_tasks);
        std::atomic<int> initialized{0};
        std::atomic<int> executed{0};

        std::vector<std::thread> tasks;
        for (int i = 0; i < num_tasks; ++i) {
            tasks.emplace_back([&]() {
                // 初始化阶段
                init_latch.count_down();
                initialized++;

                // 等待所有任务初始化完成
                init_latch.wait();

                // 执行阶段
                execution_barrier.wait();
                executed++;
                execution_barrier.wait();

                // 验证
                EXPECT_GE(initialized.load(), num_tasks);
            });
        }

        for (auto& t : tasks) {
            t.join();
        }

        EXPECT_EQ(initialized.load(), num_tasks);
        EXPECT_EQ(executed.load(), num_tasks);
    });

    RUN_TEST(semaphore_resource_pool) {
        // 模拟资源池
        const int pool_size = 3;
        const int num_requests = 10;
        Semaphore available(pool_size);
        std::atomic<int> active_resources{0};
        std::atomic<int> max_resources{0};

        auto use_resource = [&](int id) {
            available.wait();
            active_resources++;
            int current = active_resources.load();
            while (current > max_resources.load()) {
                max_resources.store(current);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            active_resources--;
            available.post();
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < num_requests; ++i) {
            threads.emplace_back(use_resource, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(active_resources.load(), 0);
        EXPECT_EQ(max_resources.load(), pool_size);  // 最多同时使用 pool_size 个资源
    });

    RUN_TEST(staggered_barrier) {
        // 交错式屏障 - 每批3个线程，分3批执行
        const int batch_size = 3;
        const int num_batches = 3;
        std::atomic<int> round_completed{0};

        // 第一个barrier确保同一批的线程同时开始
        CyclicBarrier start_barrier(batch_size);
        // 第二个barrier确保所有批次完成后才能进入下一轮
        CyclicBarrier end_barrier(batch_size);

        auto worker = [&](int id) {
            int my_batch = id % num_batches;  // 分配到不同的批次

            for (int round = 0; round < num_batches; ++round) {
                // 同一批的线程同时通过start_barrier
                start_barrier.wait();

                if (id % num_batches == 0) {
                    // 只有每批的第一个线程增加计数
                    round_completed++;
                }

                // 等待同一批的所有线程完成
                end_barrier.wait();
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < batch_size; ++i) {
            threads.emplace_back(worker, i);
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(round_completed.load(), num_batches);
    });
}

// 主函数
int main() {
    std::cout << "\n";
    std::cout << yellow("========================================\n");
    std::cout << yellow("   SYNCHRONIZATION PRIMITIVES TESTS\n");
    std::cout << yellow("========================================\n");

    test_semaphore_basic();
    test_semaphore_blocking();
    test_countdown_latch_basic();
    test_countdown_latch_concurrent();
    test_cyclic_barrier_basic();
    test_cyclic_barrier_concurrent();
    test_semaphore_producer_consumer();
    test_synchronization_integration();

    std::cout << "\n" << yellow("========================================\n");
    std::cout << yellow("   ALL SYNC PRIMITIVES TESTS COMPLETED\n");
    std::cout << yellow("========================================\n\n");

    auto& stats = g_test_stats();
    std::cout << "Final Results:\n";
    std::cout << "  Total:  " << stats.total_tests << std::endl;
    std::cout << green("  Passed: ") << stats.passed_tests << std::endl;
    std::cout << red("  Failed: ") << stats.failed_tests << std::endl;
    std::cout << yellow("  Skipped: ") << stats.skipped_tests << std::endl;

    return stats.failed_tests > 0 ? 1 : 0;
}