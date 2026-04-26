//
// page_cache.cpp
// PageCache 页缓存实现
//

#include "page_cache.h"
#include <sys/mman.h>  // mmap, munmap
#include <unistd.h>     // getpagesize

namespace cc_server {

PageCache& PageCache::get_instance() {
    static PageCache instance;
    return instance;
}

Span* PageCache::allocate_span(size_t num_pages) {
    MutexGuard lock(mutex_);

    // ========== 步骤1：在空闲链表中查找 ==========
    auto it = free_span_lists_.find(num_pages);
    if (it != free_span_lists_.end() && !it->second.empty()) {
        // 找到了！从链表中取出第一个Span
        Span* span = it->second.front();
        it->second.remove(span);

        // 从页号映射表中移除
        for (size_t i = 0; i < span->num_pages_; ++i) {
            page_span_map_.erase(span->page_id_ + i);
        }

        return span;
    }

    // ========== 步骤2：尝试分裂更大的Span ==========
    // 遍历所有更大的Span列表
    for (auto it = free_span_lists_.lower_bound(num_pages + 1);
         it != free_span_lists_.end();
         ++it) {

        if (it->second.empty()) {
            continue;  // 这个大小的链表是空的，继续找下一个
        }

        // 找到一个更大的Span
        Span* big_span = it->second.front();
        it->second.remove(big_span);

        // 计算分裂后剩余的页数
        size_t remaining_pages = big_span->num_pages_ - num_pages;

        if (remaining_pages > 0) {
            // 创建新Span（剩余部分）
            Span* new_span = new Span();
            new_span->page_id_ = big_span->page_id_ + num_pages;
            new_span->num_pages_ = remaining_pages;
            new_span->size_class_ = 0;  // 还未分配给具体SizeClass

            // 更新页号映射（新Span管理后面的页）
            for (size_t i = 0; i < new_span->num_pages_; ++i) {
                page_span_map_[new_span->page_id_ + i] = new_span;
            }

            // 把剩余的Span放回空闲链表
            free_span_lists_[remaining_pages].push_front(new_span);
        }

        // 更新被分裂的Span（只保留前面的部分）
        big_span->num_pages_ = num_pages;

        // 更新页号映射（移除这些页的映射）
        for (size_t i = 0; i < big_span->num_pages_; ++i) {
            page_span_map_.erase(big_span->page_id_ + i);
        }

        return big_span;
    }

    // ========== 步骤3：真的没有，向系统申请 ==========
    size_t page_size = getpagesize();  // 通常是4096字节（4KB）
    size_t bytes = num_pages * page_size;

    void* ptr = mmap(nullptr,                    // nullptr：让系统选择地址
                      bytes,                      // 字节数
                      PROT_READ | PROT_WRITE,    // 可读可写
                      MAP_PRIVATE | MAP_ANONYMOUS, // 私有映射，不关联文件
                      -1,                         // 文件描述符（匿名映射填-1）
                      0);                         // 文件偏移（匿名映射填0）

    if (ptr == MAP_FAILED) {
        return nullptr;  // 申请失败
    }

    // 创建新的Span
    Span* span = new Span();
    span->page_id_ = reinterpret_cast<uint64_t>(ptr) / page_size;
    span->num_pages_ = num_pages;
    span->size_class_ = 0;  // 还未分配给具体SizeClass

    return span;
}

void PageCache::free_span(Span* span) {
    MutexGuard lock(mutex_);

    // 尝试合并相邻的Span
    Span* merged = coalesce_span(span);

    // 把合并后的Span加入空闲链表
    free_span_lists_[merged->num_pages_].push_front(merged);

    // 更新页号映射
    for (size_t i = 0; i < merged->num_pages_; ++i) {
        page_span_map_[merged->page_id_ + i] = merged;
    }
}

Span* PageCache::coalesce_span(Span* span) {
    // ========== 尝试向前合并 ==========
    // 检查前面的页是否有Span
    while (true) {
        // 计算前一个页的页号
        uint64_t prev_page_id = span->page_id_ - 1;

        // 查找前一个页属于哪个Span
        auto it = page_span_map_.find(prev_page_id);
        if (it == page_span_map_.end()) {
            // 前一个页不存在或已被使用，停止向前合并
            break;
        }

        Span* prev_span = it->second;

        // 检查前一个Span是否真的在前面且空闲
        // 条件：prev_span的起始页 + 它的页数 == span的起始页
        if (prev_span->page_id_ + prev_span->num_pages_ != span->page_id_) {
            break;  // 不相邻，停止
        }

        // 合并：从page_span_map中移除前一个Span
        for (size_t i = 0; i < prev_span->num_pages_; ++i) {
            page_span_map_.erase(prev_span->page_id_ + i);
        }

        // 从空闲链表中移除前一个Span
        free_span_lists_[prev_span->num_pages_].remove(prev_span);

        // 合并：扩展span到前面
        span->page_id_ = prev_span->page_id_;
        span->num_pages_ += prev_span->num_pages_;

        // 删除前一个Span对象
        delete prev_span;
    }

    // ========== 尝试向后合并 ==========
    // 检查后面的页是否有Span
    while (true) {
        // 计算后一个页的页号
        uint64_t next_page_id = span->page_id_ + span->num_pages_;

        // 查找后一个页属于哪个Span
        auto it = page_span_map_.find(next_page_id);
        if (it == page_span_map_.end()) {
            // 后一个页不存在或已被使用，停止向后合并
            break;
        }

        Span* next_span = it->second;

        // 检查后一个Span是否真的在后面且空闲
        // 条件：span的起始页 + span的页数 == next_span的起始页
        if (span->page_id_ + span->num_pages_ != next_span->page_id_) {
            break;  // 不相邻，停止
        }

        // 从page_span_map中移除后一个Span
        for (size_t i = 0; i < next_span->num_pages_; ++i) {
            page_span_map_.erase(next_span->page_id_ + i);
        }

        // 从空闲链表中移除后一个Span
        free_span_lists_[next_span->num_pages_].remove(next_span);

        // 合并：扩展span到后面
        span->num_pages_ += next_span->num_pages_;

        // 删除后一个Span对象
        delete next_span;
    }

    return span;
}

} // namespace cc_server
