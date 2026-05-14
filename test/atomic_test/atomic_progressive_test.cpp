/**
 * Atomic Operations Progressive Test
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

// Include the ACTUAL project implementation
#include "base/lock.h"

// ============================================================================
// INDEPENDENT TEST STATISTICS
// ============================================================================

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
            std::lock_guard<std::mutex> lock(g_stats.print_mutex); \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " assertion failed: " #condition << std::endl; \
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
            std::lock_guard<std::mutex> lock(g_stats.print_mutex); \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " expected " << (expected) << " but got " << (actual) << std::endl; \
            return false; \
        } else { \
            g_stats.passed++; \
        } \
    } while(0)

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
    std::cout << "ATOMIC OPERATIONS PROGRESSIVE TESTS\n";
    std::cout << "========================================\n";

    bool all_passed = true;

    if (!test_concurrent_increment()) all_passed = false;

    std::cout << "\n========================================\n";
    std::cout << "TEST SUMMARY\n";
    std::cout << "========================================\n";
    std::cout << "Total assertions: " << g_stats.total.load() << "\n";
    std::cout << "Passed: " << g_stats.passed.load() << "\n";
    std::cout << "Failed: " << g_stats.failed.load() << "\n";
    std::cout << "========================================\n";

    return all_passed ? 0 : 1;
}