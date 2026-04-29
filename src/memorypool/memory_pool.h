//
// memory_pool.h
// 内存池 - 版本2核心组件
//
// 三层设计：
// 1. ThreadCache  - 线程本地缓存，分配无锁，极快
// 2. CentralCache - 中心缓存，所有线程共享，细粒度锁
// 3. PageCache    - 页缓存，直接和系统交互
//
// 内存池优势：
// - 减少系统调用（malloc/free）
// - 减少锁竞争（细粒度锁）
// - 减少内存碎片（Span合并）
//

#ifndef CONCURRENTCACHE_MEMORY_POOL_H
#define CONCURRENTCACHE_MEMORY_POOL_H

// 包含所有组件的头文件
#include "size_class.h"
#include "thread_cache.h"
#include <cstddef>

namespace cc_server {

// MemoryPool：对外统一接口，超过256KB直接malloc
class MemoryPool {
public:
    //
    // 分配内存
    // @param size 字节数
    // @return 分配好的内存指针
    //
    static void* allocate(size_t size) {
        // 超过256KB直接找系统要（不适合池化）
        if (size > SizeClass::kSizeClasses[SizeClass::kNumClasses - 1]) {
            return malloc(size);
        }

        // 找到合适的SizeClass
        size_t class_index = SizeClass::get_index(size);
        if (class_index == static_cast<size_t>(-1)) {
            return malloc(size);
        }

        // 从ThreadCache获取
        return ThreadCache::get_instance()->allocate(size);
    }

    //
    // 释放内存
    // @param ptr 内存指针
    // @param size 原始请求的大小
    //
    static void deallocate(void* ptr, size_t size) {
        // 超过256KB的直接free
        if (size > SizeClass::kSizeClasses[SizeClass::kNumClasses - 1]) {
            free(ptr);
            return;
        }

        // 找到合适的SizeClass
        size_t class_index = SizeClass::get_index(size);
        if (class_index == static_cast<size_t>(-1)) {
            free(ptr);
            return;
        }

        // 归还给ThreadCache
        ThreadCache::get_instance()->deallocate(ptr, size);
    }
};

// 便捷宏
#define MALLOC(size) cc_server::MemoryPool::allocate(size)
#define FREE(ptr, size) cc_server::MemoryPool::deallocate(ptr, size)

} // namespace cc_server

#endif // CONCURRENTCACHE_MEMORY_POOL_H
