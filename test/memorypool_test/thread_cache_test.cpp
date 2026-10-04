// ThreadCache 批量归还的回归测试。
//
// deallocate 攒够 33 个就触发一次归还，"还多少"直接决定线程缓存有没有意义：
// 一次性清空等于每次攒够都要回 CentralCache 抢锁批量取，本地缓存形同虚设。
// 这里不计时、不起线程，只用 cached_object_count 把每一步的增减读出来。

#include <cstddef>
#include <iostream>
#include <unordered_set>
#include <vector>

#include "../trace/test_assertions.h"
#include "memorypool/size_class.h"
#include "memorypool/thread_cache.h"

namespace cc_server {
namespace testing {

namespace {

constexpr size_t kClassSize = 32;   // SizeClass 第 2 级
constexpr size_t kObjectCount = 100;

void* checked_alloc(ThreadCache* tc) {
    void* obj = tc->allocate(kClassSize);
    EXPECT_TRUE(obj != nullptr);
    return obj;
}

}  // namespace

// 核心回归：一次归还不能把本地缓存掏空，至多一半。
void test_thread_cache_bulk_free_returns_at_most_half() {
    ThreadCache* tc = ThreadCache::get_instance();
    const size_t class_index = SizeClass::get_index(kClassSize);
    ASSERT_EQ(class_index, static_cast<size_t>(2));

    std::vector<void*> objs;
    objs.reserve(kObjectCount);
    for (size_t i = 0; i < kObjectCount; ++i) {
        objs.push_back(checked_alloc(tc));
    }

    bool saw_return = false;
    for (void* obj : objs) {
        const size_t before = tc->cached_object_count(class_index);
        tc->deallocate(obj, kClassSize);
        const size_t after = tc->cached_object_count(class_index);

        // push 会让计数 +1，因此没触发归还时 after 必然等于 before + 1
        if (after < before + 1) {
            saw_return = true;
            EXPECT_GE(after * 2, before);
        }
    }

    // 100 个对象逐个释放，途中必然越过 32 的阈值
    EXPECT_TRUE(saw_return);
}

// 护栏：释放后再分配，指针不能重复发放（FreeList 批量压入/弹出串链的直接后果）
void test_thread_cache_free_then_realloc_stays_consistent() {
    ThreadCache* tc = ThreadCache::get_instance();

    std::vector<void*> first_gen;
    first_gen.reserve(kObjectCount);
    for (size_t i = 0; i < kObjectCount; ++i) {
        first_gen.push_back(checked_alloc(tc));
    }
    EXPECT_EQ(std::unordered_set<void*>(first_gen.begin(), first_gen.end()).size(),
              first_gen.size());

    for (void* obj : first_gen) {
        tc->deallocate(obj, kClassSize);
    }

    std::vector<void*> second_gen;
    second_gen.reserve(kObjectCount);
    for (size_t i = 0; i < kObjectCount; ++i) {
        second_gen.push_back(checked_alloc(tc));
    }
    EXPECT_EQ(std::unordered_set<void*>(second_gen.begin(), second_gen.end()).size(),
              second_gen.size());
}

void run_all_thread_cache_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Thread Cache Tests\n";
    std::cout << "========================================\n\n";

    test_thread_cache_bulk_free_returns_at_most_half();
    test_thread_cache_free_then_realloc_stays_consistent();

    std::cout << "\n========================================\n";
    std::cout << "All Thread Cache Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
