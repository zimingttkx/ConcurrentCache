//
// thread_cache.h
// ThreadCache 线程本地缓存
//
// ThreadCache职责：
// 1. 每个线程有自己的ThreadCache实例（thread_local）
// 2. 分配时直接在自己的缓存里拿，不需要锁
// 3. 缓存不够了，向CentralCache批量获取
// 4. 缓存太多了，归还给CentralCache
//
// 为什么ThreadCache要批量获取/归还？
// - 减少锁操作次数（锁有开销）
// - 批量操作均摊成本
//
// thread_local 的含义：
// - 每个线程看到这个变量是不同的实例
// - 线程A的ThreadCache和线程B的ThreadCache互不影响
// - 所以分配和释放完全不需要加锁！
//

#ifndef CONCURRENTCACHE_THREAD_CACHE_H
#define CONCURRENTCACHE_THREAD_CACHE_H

#include <cstddef>
#include <vector>
#include "free_list.h"
#include "size_class.h"

namespace cc_server {

class CentralCache;

class ThreadCache {
public:

    // 禁止拷贝

    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;
    // 获取当前线程的ThreadCache实例
    static ThreadCache* get_instance();

    //
    // 分配size字节
    // @param size 要分配的字节数
    // @return 分配好的内存指针
    //
    void* allocate(size_t size);

    //
    // 释放内存
    // @param obj 对象指针
    // @param size 对象大小
    //
    void deallocate(void* obj, size_t size);

private:
    ThreadCache() {
        // 初始化每个SizeClass对应的FreeList
        free_lists_.resize(SizeClass::kNumClasses);
    }

    //
    // 从CentralCache获取一批对象
    // @param class_index SizeClass索引
    //
    void fetch_from_central(size_t class_index);

    //
    // 归还过多的小块给CentralCache
    // @param class_index SizeClass索引
    //
    void return_to_central(size_t class_index);

    // 每个SizeClass对应的FreeList
    std::vector<FreeList> free_lists_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_THREAD_CACHE_H
