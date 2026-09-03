//
// thread_cache.cpp
// ThreadCache 线程本地缓存实现
//

#include "thread_cache.h"

#include <algorithm>
#include <cstdlib>

#include "central_cache.h"

namespace cc_server {

ThreadCache* ThreadCache::get_instance() {
    // 每线程一个实例（thread_local）：分配/释放路径完全无锁。
    // 修复 P2（链接地雷）：此前 get_instance 只有声明、全仓库无定义，
    // MemoryPool::allocate/deallocate 一旦被调用即链接失败。
    static thread_local ThreadCache instance;
    return &instance;
}

ThreadCache::ThreadCache() {
    // 初始化每个SizeClass对应的FreeList
    free_lists_.resize(SizeClass::kNumClasses);
}

ThreadCache::~ThreadCache() {
    // 线程退出时，把每个 SizeClass 缓存的空闲对象全部归还 CentralCache，
    // 确保内存池可回收（修复 P1-3：此前缺少析构，线程死亡时缓存对象泄漏）。
    for (size_t i = 0; i < free_lists_.size(); ++i) {
        FreeList& free_list = free_lists_[i];
        if (free_list.empty()) continue;
        return_to_central(i);
    }
}

void* ThreadCache::allocate(const size_t size) {
    // 找到对应的SizeClass索引
    const size_t class_index = SizeClass::get_index(size);
    if (class_index == static_cast<size_t>(-1)) {
        // 超过256KB，直接malloc
        return malloc(size);
    }

    FreeList& free_list = free_lists_[class_index];

    // 步骤1：从FreeList获取
    if (!free_list.empty()) {
        return free_list.pop();
    }

    // 步骤2：FreeList为空，从CentralCache获取
    fetch_from_central(class_index);

    // 获取后再次尝试
    if (!free_list.empty()) {
        return free_list.pop();
    }

    return nullptr;  // 仍然失败
}

void ThreadCache::deallocate(void* obj, size_t size) {
    // 找到对应的SizeClass索引
    const size_t class_index = SizeClass::get_index(size);
    if (class_index == static_cast<size_t>(-1)) {
        // 超过256KB，直接free
        free(obj);
        return;
    }

    FreeList& free_list = free_lists_[class_index];

    // 把对象放回FreeList
    free_list.push(obj);

    // 如果FreeList过多，归还给CentralCache
    // 阈值：FreeList大小超过一定数量时归还
    // 这样可以避免一个线程占用太多内存
    if (free_list.size() > 32) {
        return_to_central(class_index);
    }
}

void ThreadCache::fetch_from_central(size_t class_index) {
    // 一次获取多个对象
    void* objs[256];
    int count = 0;

    // 从CentralCache获取
    for (int i = 0; i < 256; ++i) {
        objs[i] = CentralCache::get_instance().allocate(class_index);
        if (objs[i] == nullptr) {
            break;  // 获取失败
        }
        count++;
    }

    // 把获取的对象放入FreeList（包括objs[0]，调用者通过pop()获取）
    for (int i = 0; i < count; ++i) {
        free_lists_[class_index].push(objs[i]);
    }
}

void ThreadCache::return_to_central(size_t class_index) {
    FreeList& free_list = free_lists_[class_index];

    // 归还一半（析构场景下 free_list 全部对象也会被一次性归还：见 ~ThreadCache）
    size_t return_count = free_list.size() / 2;
    if (return_count == 0) return_count = 1;

    // pop_batch 单次最多 256 个，循环归还避免一次 pop 超限
    while (free_list.size() > 0) {
        size_t batch = std::min<size_t>(free_list.size(), 256);
        void* objs[256];
        size_t actual_count = free_list.pop_batch(objs, batch);

        for (size_t i = 0; i < actual_count; ++i) {
            CentralCache::get_instance().deallocate(objs[i], class_index);
        }

        // 若实际归还数量小于请求数量，说明已无更多对象，避免死循环
        if (actual_count < batch) break;
    }
}

} // namespace cc_server
