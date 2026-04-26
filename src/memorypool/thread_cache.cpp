//
// thread_cache.cpp
// ThreadCache 线程本地缓存实现
//

#include "thread_cache.h"

#include <cstdlib>

#include "central_cache.h"

namespace cc_server {

ThreadCache* ThreadCache::get_instance() {
    // C++11 thread_local 保证每个线程有独立实例
    static thread_local ThreadCache instance;
    return &instance;
}

void* ThreadCache::allocate(const size_t size) {
    // 找到对应的SizeClass索引
    const size_t class_index = SizeClass::get_index(size);
    if (class_index == static_cast<size_t>(-1)) {
        // 超过256KB，直接malloc
        return malloc(size);
    }

    FreeList& free_list = free_lists_[class_index];

    // ========== 步骤1：从FreeList获取 ==========
    if (!free_list.empty()) {
        return free_list.pop();
    }

    // ========== 步骤2：FreeList为空，从CentralCache获取 ==========
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

    // 从CentralCache获取
    for (auto & obj : objs) {
        obj = CentralCache::get_instance().allocate(class_index);
        if (obj == nullptr) {
            break;  // 获取失败
        }
    }

    // 把获取的对象放入FreeList
    for (int i = 1; i < 256; ++i) {  // 第一个返回给调用者
        free_lists_[class_index].push(objs[i]);
    }
}

void ThreadCache::return_to_central(size_t class_index) {
    FreeList& free_list = free_lists_[class_index];

    // 归还一半
    size_t return_count = free_list.size() / 2;
    if (return_count == 0) return_count = 1;

    void* objs[256];
    size_t actual_count = free_list.pop_batch(objs, return_count);

    // 逐个归还给CentralCache
    for (size_t i = 0; i < actual_count; ++i) {
        CentralCache::get_instance().deallocate(objs[i], class_index);
    }
}

} // namespace cc_server
