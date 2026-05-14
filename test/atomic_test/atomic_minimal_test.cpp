/**
 * Atomic Operations Minimal Test
 */

#include <iostream>
#include <thread>
#include <atomic>
#include "base/lock.h"

int main() {
    std::cout << "Starting minimal atomic test\n";

    cc_server::AtomicInteger atomic(42);
    std::cout << "AtomicInteger created with value: " << atomic.load() << "\n";

    int old = atomic.fetch_add(8);
    std::cout << "fetch_add returned: " << old << ", new value: " << atomic.load() << "\n";

    std::cout << "Creating threads...\n";

    std::atomic<int> counter{0};
    std::thread t1([&]() {
        for (int i = 0; i < 1000; ++i) {
            counter.fetch_add(1);
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 1000; ++i) {
            counter.fetch_add(1);
        }
    });

    t1.join();
    t2.join();

    std::cout << "Final counter: " << counter.load() << " (expected 2000)\n";
    std::cout << "Minimal atomic test passed!\n";

    return 0;
}