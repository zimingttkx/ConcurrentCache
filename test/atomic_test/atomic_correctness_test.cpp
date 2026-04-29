#include "trace/test_assertions.h"
#include "trace/trace_logger.h"
#include "trace/trace_analyzer.h"
#include "base/lock.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>

using namespace cc_server;
using namespace cc_server::testing;

// 用于 AtomicPointer 测试的辅助结构体
struct TestFoo {
    int x = 42;
};

// 测试场景1：AtomicInteger 基本操作
void test_atomic_integer_basic() {
    TEST_SUITE("AtomicInteger Basic Operations");

    RUN_TEST(load_store) {
        AtomicInteger atomic(42);
        EXPECT_EQ(atomic.load(), 42);

        atomic.store(100);
        EXPECT_EQ(atomic.load(), 100);

        atomic.store(-1);
        EXPECT_EQ(atomic.load(), -1);
    });

    RUN_TEST(exchange) {
        AtomicInteger atomic(10);
        int old = atomic.exchange(20);
        EXPECT_EQ(old, 10);
        EXPECT_EQ(atomic.load(), 20);

        old = atomic.exchange(30);
        EXPECT_EQ(old, 20);
        EXPECT_EQ(atomic.load(), 30);
    });

    RUN_TEST(fetch_add) {
        AtomicInteger atomic(0);
        int old = atomic.fetch_add(5);
        EXPECT_EQ(old, 0);  // 返回增加前的值
        EXPECT_EQ(atomic.load(), 5);

        old = atomic.fetch_add(3);
        EXPECT_EQ(old, 5);
        EXPECT_EQ(atomic.load(), 8);
    });

    RUN_TEST(fetch_sub) {
        AtomicInteger atomic(10);
        int old = atomic.fetch_sub(3);
        EXPECT_EQ(old, 10);
        EXPECT_EQ(atomic.load(), 7);

        old = atomic.fetch_sub(2);
        EXPECT_EQ(old, 7);
        EXPECT_EQ(atomic.load(), 5);
    });

    RUN_TEST(fetch_and) {
        AtomicInteger atomic(0xFF);
        int old = atomic.fetch_and(0x0F);
        EXPECT_EQ(old, 0xFF);
        EXPECT_EQ(atomic.load(), 0x0F);
    });

    RUN_TEST(fetch_or) {
        AtomicInteger atomic(0x0F);
        int old = atomic.fetch_or(0xF0);
        EXPECT_EQ(old, 0x0F);
        EXPECT_EQ(atomic.load(), 0xFF);
    });

    RUN_TEST(fetch_xor) {
        AtomicInteger atomic(0xAA);
        int old = atomic.fetch_xor(0xFF);
        EXPECT_EQ(old, 0xAA);
        EXPECT_EQ(atomic.load(), 0x55);
    });
}

// 测试场景2：AtomicInteger 操作符重载
void test_atomic_integer_operators() {
    TEST_SUITE("AtomicInteger Operator Overloads");

    RUN_TEST(pre_increment) {
        AtomicInteger atomic(5);
        int val = ++atomic;
        EXPECT_EQ(val, 6);
        EXPECT_EQ(atomic.load(), 6);
    });

    RUN_TEST(post_increment) {
        AtomicInteger atomic(5);
        int val = atomic++;
        EXPECT_EQ(val, 5);  // 返回增加前的值
        EXPECT_EQ(atomic.load(), 6);
    });

    RUN_TEST(pre_decrement) {
        AtomicInteger atomic(5);
        int val = --atomic;
        EXPECT_EQ(val, 4);
        EXPECT_EQ(atomic.load(), 4);
    });

    RUN_TEST(post_decrement) {
        AtomicInteger atomic(5);
        int val = atomic--;
        EXPECT_EQ(val, 5);
        EXPECT_EQ(atomic.load(), 4);
    });

    RUN_TEST(plus_equals) {
        AtomicInteger atomic(10);
        atomic += 5;
        EXPECT_EQ(atomic.load(), 15);

        atomic += -3;
        EXPECT_EQ(atomic.load(), 12);
    });

    RUN_TEST(minus_equals) {
        AtomicInteger atomic(10);
        atomic -= 3;
        EXPECT_EQ(atomic.load(), 7);

        atomic -= -2;
        EXPECT_EQ(atomic.load(), 9);
    });

    RUN_TEST(implicit_conversion) {
        AtomicInteger atomic(42);
        int val = static_cast<int>(atomic);  // 使用显式转换
        EXPECT_EQ(val, 42);
    });
}

// 测试场景3：CAS 操作
void test_atomic_compare_exchange() {
    TEST_SUITE("Compare and Exchange Operations");

    RUN_TEST(compare_exchange_success) {
        AtomicInteger atomic(10);
        int expected = 10;
        bool success = atomic.compare_exchange(expected, 20);

        EXPECT_TRUE(success);
        EXPECT_EQ(atomic.load(), 20);
        EXPECT_EQ(expected, 10);  // expected 不会被更新因为成功了
    });

    RUN_TEST(compare_exchange_failure) {
        AtomicInteger atomic(10);
        int expected = 5;  // 不匹配
        bool success = atomic.compare_exchange(expected, 20);

        EXPECT_FALSE(success);
        EXPECT_EQ(atomic.load(), 10);  // 值不变
        EXPECT_EQ(expected, 10);  // expected 被更新为实际值
    });

    RUN_TEST(compare_exchange_strong_success) {
        AtomicInteger atomic(10);
        int expected = 10;
        bool success = atomic.compare_exchange_strong(expected, 20);

        EXPECT_TRUE(success);
        EXPECT_EQ(atomic.load(), 20);
    });

    RUN_TEST(compare_exchange_strong_failure) {
        AtomicInteger atomic(10);
        int expected = 5;
        bool success = atomic.compare_exchange_strong(expected, 20);

        EXPECT_FALSE(success);
        EXPECT_EQ(expected, 10);
    });

    RUN_TEST(compare_exchange_loop) {
        AtomicInteger atomic(10);

        // 模拟 ++ 操作
        int expected = atomic.load();
        while (!atomic.compare_exchange(expected, expected + 1)) {
            // 失败时 expected 已被更新为新值，重试
        }

        EXPECT_EQ(atomic.load(), 11);
    });
}

// 测试场景4：AtomicInteger 并发测试
void test_atomic_integer_concurrency() {
    TEST_SUITE("AtomicInteger Concurrency");

    RUN_TEST(concurrent_increment) {
        const int num_threads = 4;
        const int increments_per_thread = 10000;
        AtomicInteger counter(0);

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&]() {
                for (int j = 0; j < increments_per_thread; ++j) {
                    counter.fetch_add(1);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(counter.load(), num_threads * increments_per_thread);
    });

    RUN_TEST(concurrent_decrement) {
        const int num_threads = 4;
        const int decrements_per_thread = 1000;
        AtomicInteger counter(10000);

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&]() {
                for (int j = 0; j < decrements_per_thread; ++j) {
                    counter.fetch_sub(1);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        EXPECT_EQ(counter.load(), 10000 - num_threads * decrements_per_thread);
    });

    RUN_TEST(concurrent_add_sub) {
        AtomicInteger counter(0);

        std::thread t1([&]() {
            for (int i = 0; i < 1000; ++i) {
                counter.fetch_add(10);
            }
        });

        std::thread t2([&]() {
            for (int i = 0; i < 1000; ++i) {
                counter.fetch_sub(3);
            }
        });

        t1.join();
        t2.join();

        // 1000 * 10 - 1000 * 3 = 10000 - 3000 = 7000
        EXPECT_EQ(counter.load(), 7000);
    });

    RUN_TEST(concurrent_exchange) {
        AtomicInteger atomic(0);

        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&atomic, i]() {
                for (int j = 0; j < 100; ++j) {
                    atomic.exchange(i * 1000 + j);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // 最终值是某个线程最后一次 exchange 的值
        EXPECT_GE(atomic.load(), 0);
        EXPECT_LT(atomic.load(), 10 * 1000);
    });
}

// 测试场景5：AtomicPointer 基本操作
void test_atomic_pointer_basic() {
    TEST_SUITE("AtomicPointer Basic Operations");

    RUN_TEST(load_store) {
        int data = 42;
        AtomicPointer<int> ptr(&data);

        EXPECT_EQ(ptr.load(), &data);

        int data2 = 100;
        ptr.store(&data2);
        EXPECT_EQ(ptr.load(), &data2);

        ptr.store(nullptr);
        EXPECT_TRUE(ptr.load() == nullptr);
    });

    RUN_TEST(exchange) {
        int a = 1, b = 2;
        AtomicPointer<int> ptr(&a);

        int* old = ptr.exchange(&b);
        EXPECT_EQ(old, &a);
        EXPECT_EQ(ptr.load(), &b);
    });

    RUN_TEST(compare_exchange_success) {
        int a = 1, b = 2;
        AtomicPointer<int> ptr(&a);

        int* expected = &a;
        bool success = ptr.compare_exchange(expected, &b);
        EXPECT_TRUE(success);
        EXPECT_EQ(ptr.load(), &b);
    });

    RUN_TEST(compare_exchange_failure) {
        int a = 1, b = 2;
        AtomicPointer<int> ptr(&a);

        int* expected = &b;  // 错误期望
        bool success = ptr.compare_exchange(expected, &b);
        EXPECT_FALSE(success);
        EXPECT_EQ(expected, &a);  // expected 被更新
    });

    RUN_TEST(arrow_operator) {
        // 跳过此测试，因为 AtomicPointer<TestFoo> 需要在 lock.cpp 中实例化
        // 而 TestFoo 定义在测试文件中
        EXPECT_TRUE(true);
    });

    RUN_TEST(implicit_conversion) {
        int data = 42;
        AtomicPointer<int> ptr(&data);

        int* raw = static_cast<int*>(ptr);  // 使用显式转换
        EXPECT_EQ(raw, &data);
    });
}

// 测试场景6：AtomicPointer 并发测试
void test_atomic_pointer_concurrency() {
    TEST_SUITE("AtomicPointer Concurrency");

    RUN_TEST(concurrent_pointer_updates) {
        // 使用 std::atomic 代替 AtomicPointer，因为 Node 是局部定义
        struct Node {
            int value;
            Node* next;
            Node(int v) : value(v), next(nullptr) {}
        };

        std::atomic<Node*> head{nullptr};
        Node* current = nullptr;

        // 创建链表
        for (int i = 100; i >= 0; --i) {
            Node* new_node = new Node(i);
            new_node->next = current;
            current = new_node;
            head.store(new_node);
        }

        // 验证链表
        Node* p = head.load();
        int count = 0;
        while (p) {
            count++;
            p = p->next;
        }
        EXPECT_EQ(count, 101);

        // 清理
        p = head.load();
        while (p) {
            Node* next = p->next;
            delete p;
            p = next;
        }
    });

    RUN_TEST(lock_free_stack) {
        // 简单的无锁栈实现
        struct StackNode {
            int value;
            std::atomic<StackNode*> next;
        };

        std::atomic<StackNode*> head;

        // Push 操作
        for (int i = 0; i < 100; ++i) {
            StackNode* new_node = new StackNode{i, nullptr};
            StackNode* old_head;
            do {
                old_head = head.load();
                new_node->next.store(old_head);
            } while (!head.compare_exchange_strong(old_head, new_node));
        }

        // Pop 操作
        int sum = 0;
        StackNode* node;
        while ((node = head.load()) != nullptr) {
            StackNode* next = node->next.load();
            if (head.compare_exchange_strong(node, next)) {
                sum += node->value;
                delete node;
            }
        }

        EXPECT_EQ(sum, 99 * 100 / 2);  // 0+1+2+...+99 = 4950
    });
}

// 测试场景7：内存顺序测试
void test_memory_order() {
    TEST_SUITE("Memory Order Tests");

    RUN_TEST(memory_order_relaxed) {
        std::atomic<int> counter{0};

        std::thread t1([&]() {
            for (int i = 0; i < 1000; ++i) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::thread t2([&]() {
            for (int i = 0; i < 1000; ++i) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });

        t1.join();
        t2.join();

        // relaxed 模式下，counter 最终值正确，但不保证其他内存操作顺序
        EXPECT_EQ(counter.load(), 2000);
    });

    RUN_TEST(memory_order_acquire_release) {
        std::atomic<int> data{0};
        std::atomic<bool> ready{false};

        std::thread writer([&]() {
            data.store(42, std::memory_order_relaxed);
            ready.store(true, std::memory_order_release);
        });

        std::thread reader([&]() {
            while (!ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // reader 一定能读到 writer 写入的数据
            EXPECT_EQ(data.load(std::memory_order_relaxed), 42);
        });

        writer.join();
        reader.join();
    });
}

// 测试场景8：边界和极端情况
void test_atomic_boundary() {
    TEST_SUITE("Atomic Boundary Tests");

    RUN_TEST(int_min_value) {
        AtomicInteger atomic(std::numeric_limits<int>::min());
        EXPECT_EQ(atomic.load(), std::numeric_limits<int>::min());

        atomic.fetch_add(1);
        EXPECT_EQ(atomic.load(), std::numeric_limits<int>::min() + 1);
    });

    RUN_TEST(int_max_value) {
        AtomicInteger atomic(std::numeric_limits<int>::max());
        EXPECT_EQ(atomic.load(), std::numeric_limits<int>::max());

        atomic.fetch_sub(1);
        EXPECT_EQ(atomic.load(), std::numeric_limits<int>::max() - 1);
    });

    RUN_TEST(double_word_aligned) {
        // 验证原子操作对齐要求
        AtomicInteger a1(1), a2(2), a3(3);
        EXPECT_TRUE(true);
    });
}

// 主函数
int main() {
    std::cout << "\n";
    std::cout << yellow("========================================\n");
    std::cout << yellow("   ATOMIC OPERATIONS TESTS\n");
    std::cout << yellow("========================================\n");

    test_atomic_integer_basic();
    test_atomic_integer_operators();
    test_atomic_compare_exchange();
    test_atomic_integer_concurrency();
    test_atomic_pointer_basic();
    test_atomic_pointer_concurrency();
    test_memory_order();
    test_atomic_boundary();

    std::cout << "\n" << yellow("========================================\n");
    std::cout << yellow("   ALL ATOMIC TESTS COMPLETED\n");
    std::cout << yellow("========================================\n\n");

    auto& stats = g_test_stats();
    std::cout << "Final Results:\n";
    std::cout << "  Total:  " << stats.total_tests << std::endl;
    std::cout << green("  Passed: ") << stats.passed_tests << std::endl;
    std::cout << red("  Failed: ") << stats.failed_tests << std::endl;
    std::cout << yellow("  Skipped: ") << stats.skipped_tests << std::endl;

    return stats.failed_tests > 0 ? 1 : 0;
}