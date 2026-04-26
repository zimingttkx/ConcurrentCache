//
// central_cache.h
// CentralCache 中心缓存 - 版本2核心组件
//
// CentralCache职责：
// 1. 管理所有SizeClass对应的SpanList
// 2. ThreadCache申请时，分配一个小块
// 3. ThreadCache释放时，收回小块
// 4. Span空了或者太多了就还给PageCache
//
// 锁设计：
// - 每个SizeClass有一把独立的锁
// - 这样不同SizeClass的访问互不影响
// - 减少了锁竞争
//

#ifndef CONCURRENTCACHE_CENTRAL_CACHE_H
#define CONCURRENTCACHE_CENTRAL_CACHE_H

#include <cstddef>
#include <vector>
#include "span.h"
#include "size_class.h"
#include "lock.h"

namespace cc_server {

class CentralCache {
public:
    // 获取单例实例
    static CentralCache& get_instance();

    //
    // 分配一个size_class对应大小的对象
    // @param class_index SizeClass索引
    // @return 分配好的内存指针
    //
    void* allocate(size_t class_index);

    //
    // 释放一个对象
    // @param obj 对象指针
    // @param class_index SizeClass索引
    //
    void deallocate(void* obj, size_t class_index);

private:
    CentralCache() {
        // 初始化SpanLists和locks
        span_lists_.resize(SizeClass::kNumClasses);
        locks_.resize(SizeClass::kNumClasses);
    }

    // 禁用拷贝
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    //
    // 从PageCache获取新的Span
    // @param class_index SizeClass索引
    // @return 分配好的Span
    //
    Span* fetch_from_page_cache(size_t class_index);

    // 每个SizeClass有独立的SpanList
    std::vector<SpanList> span_lists_;

    // 每个SizeClass有独立的锁（细粒度锁）
    std::vector<Mutex> locks_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CENTRAL_CACHE_H
