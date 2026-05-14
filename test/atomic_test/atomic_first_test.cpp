/**
 * Atomic Operations First Test Only
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

int main() {
    std::cout << "========================================\n";
    std::cout << "ATOMIC FIRST TEST\n";
    std::cout << "========================================\n";

    bool all_passed = true;

    if (!test_atomic_load_store()) all_passed = false;

    std::cout << "\n========================================\n";
    std::cout << "TEST SUMMARY\n";
    std::cout << "========================================\n";
    std::cout << "Total: " << g_stats.total.load() << "\n";
    std::cout << "Passed: " << g_stats.passed.load() << "\n";
    std::cout << "Failed: " << g_stats.failed.load() << "\n";
    std::cout << "========================================\n";

    return all_passed ? 0 : 1;
}