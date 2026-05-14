/**
 * Atomic Operations Memory Order Test
 */

#include <iostream>
#include <thread>
#include <atomic>
#include "base/lock.h"

int main() {
    std::cout << "========================================\n";
    std::cout << "ATOMIC MEMORY ORDER TEST\n";
    std::cout << "========================================\n";

    std::atomic<int> counter{0};

    std::thread t1([&]() {
        for (int i = 0; i < 10000; ++i) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 10000; ++i) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    t1.join();
    t2.join();

    std::cout << "Final counter: " << counter.load() << " (expected 20000)\n";

    if (counter.load() == 20000) {
        std::cout << "Memory order test passed!\n";
        return 0;
    } else {
        std::cout << "Memory order test FAILED!\n";
        return 1;
    }
}