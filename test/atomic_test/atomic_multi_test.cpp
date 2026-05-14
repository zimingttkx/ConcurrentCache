/**
 * Atomic Operations Multi-Test
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <limits>

#include "base/lock.h"

struct TestStats {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> passed{0};
    std::atomic<uint64_t> failed{0};
    std::mutex print_mutex;
};

TestStats g_stats;

#define TEST_ASSERT(condition) \
    do { \
        g_stats.total++; \
        if (!(condition)) { \
            g_stats.failed++; \
            return false; \
        } else { \
            g_stats.passed++; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(actual, expected) \
    do { \
        g_stats.total++; \
        if ((actual) != (expected)) { \
            g_stats.failed++; \
            return false; \
        } else { \
            g_stats.passed++; \
        } \
    } while(0)

bool test_atomic_load_store() {
    std::cout << "\n[TEST] AtomicInteger load/store\n";
    cc_server::AtomicInteger atomic(42);
    TEST_ASSERT_EQ(atomic.load(), 42);
    atomic.store(100);
    TEST_ASSERT_EQ(atomic.load(), 100);
    atomic.store(-1);
    TEST_ASSERT_EQ(atomic.load(), -1);
    atomic.store(0);
    TEST_ASSERT_EQ(atomic.load(), 0);
    return true;
}

bool test_atomic_exchange() {
    std::cout << "\n[TEST] AtomicInteger exchange\n";
    cc_server::AtomicInteger atomic(10);
    int old = atomic.exchange(20);
    TEST_ASSERT_EQ(old, 10);
    TEST_ASSERT_EQ(atomic.load(), 20);
    old = atomic.exchange(30);
    TEST_ASSERT_EQ(old, 20);
    TEST_ASSERT_EQ(atomic.load(), 30);
    old = atomic.exchange(0);
    TEST_ASSERT_EQ(old, 30);
    TEST_ASSERT_EQ(atomic.load(), 0);
    return true;
}

bool test_atomic_operators() {
    std::cout << "\n[TEST] AtomicInteger operators\n";
    cc_server::AtomicInteger atomic(5);

    int val = ++atomic;
    TEST_ASSERT_EQ(val, 6);
    TEST_ASSERT_EQ(atomic.load(), 6);

    val = atomic++;
    TEST_ASSERT_EQ(val, 6);
    TEST_ASSERT_EQ(atomic.load(), 7);

    val = --atomic;
    TEST_ASSERT_EQ(val, 6);
    TEST_ASSERT_EQ(atomic.load(), 6);

    val = atomic--;
    TEST_ASSERT_EQ(val, 6);
    TEST_ASSERT_EQ(atomic.load(), 5);

    atomic += 5;
    TEST_ASSERT_EQ(atomic.load(), 10);

    atomic += -3;
    TEST_ASSERT_EQ(atomic.load(), 7);

    atomic -= 3;
    TEST_ASSERT_EQ(atomic.load(), 4);

    atomic -= -2;
    TEST_ASSERT_EQ(atomic.load(), 6);

    cc_server::AtomicInteger atomic2(42);
    int conv_val = static_cast<int>(atomic2);
    TEST_ASSERT_EQ(conv_val, 42);

    return true;
}

bool test_compare_exchange_success() {
    std::cout << "\n[TEST] Compare exchange success\n";
    cc_server::AtomicInteger atomic(10);
    int expected = 10;
    bool success = atomic.compare_exchange(expected, 20);
    TEST_ASSERT(success);
    TEST_ASSERT_EQ(atomic.load(), 20);
    TEST_ASSERT_EQ(expected, 10);
    return true;
}

bool test_concurrent_increment() {
    std::cout << "\n[TEST] Concurrent increment\n";
    const int num_threads = 8;
    const int increments = 10000;
    cc_server::AtomicInteger counter(0);

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < increments; ++j) {
                counter.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    TEST_ASSERT_EQ(counter.load(), num_threads * increments);
    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "ATOMIC MULTI TEST\n";
    std::cout << "========================================\n";

    bool all_passed = true;

    if (!test_atomic_load_store()) all_passed = false;
    if (!test_atomic_exchange()) all_passed = false;
    if (!test_atomic_operators()) all_passed = false;
    if (!test_compare_exchange_success()) all_passed = false;
    if (!test_concurrent_increment()) all_passed = false;

    std::cout << "\n========================================\n";
    std::cout << "TEST SUMMARY\n";
    std::cout << "========================================\n";
    std::cout << "Total: " << g_stats.total.load() << "\n";
    std::cout << "Passed: " << g_stats.passed.load() << "\n";
    std::cout << "Failed: " << g_stats.failed.load() << "\n";
    std::cout << "========================================\n";

    return all_passed ? 0 : 1;
}