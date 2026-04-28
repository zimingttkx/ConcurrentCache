//
// page_cache.h
// PageCache 页缓存 - 版本2核心组件
//
// PageCache职责：
// 1. 管理所有Span，按页数组织
// 2. CentralCache申请Span时，分配给它
// 3. CentralCache释放Span时，收回来并尝试合并
//
// 合并相邻Span可以减少内存碎片
//

#ifndef CONCURRENTCACHE_PAGE_CACHE_H
#define CONCURRENTCACHE_PAGE_CACHE_H

#include <cstdint>
#include <cstddef>
#include <map>
#include "span.h"
#include "base/lock.h"

namespace cc_server {

class PageCache {
public:
    // 获取单例实例
    static PageCache& get_instance();

    //
    // 申请n个页的Span
    // @param num_pages 页数
    // @return 分配好的Span指针
    //
    Span* allocate_span(size_t num_pages);

    //
    // 释放一个Span
    // @param span 要释放的Span
    //
    void free_span(Span* span);

private:
    PageCache() = default;

    // 禁用拷贝
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    //
    // 尝试合并相邻的空闲Span
    // @param span 刚释放的Span
    // @return 合并后的Span（可能比原来的大）
    //
    Span* coalesce_span(Span* span);

    // 页号到Span的映射（用于合并时查找相邻Span）
    std::map<uint64_t, Span*> page_span_map_;

    // 按页数分类的Span链表（key是页数）
    std::map<size_t, SpanList> free_span_lists_;

    // 全局锁（PageCache只需要一把全局锁）
    Mutex mutex_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_PAGE_CACHE_H
