//
// central_cache.cpp
// CentralCache 中心缓存实现
//

#include "central_cache.h"
#include "page_cache.h"
#include <unistd.h>  // getpagesize

namespace cc_server {

CentralCache& CentralCache::get_instance() {
    static CentralCache instance;
    return instance;
}

void* CentralCache::allocate(size_t class_index) {
    // 获取对应SizeClass的锁（细粒度锁，不同SizeClass可以并行）
    MutexGuard lock(locks_[class_index]);

    SpanList& span_list = span_lists_[class_index];

    // 步骤1：从SpanList获取对象
    if (!span_list.empty()) {
        Span* span = span_list.front();

        if (span->free_count_ > 0) {
            // Span还有空闲对象，取出一个
            void* obj = span->free_list_;
            span->free_list_ = *reinterpret_cast<void**>(span->free_list_);
            span->free_count_--;

            return obj;
        }

        // Span没有空闲对象了（但还在链表里），需要获取新的Span
        span_list.remove(span);
    }

    // 步骤2：从PageCache获取新Span
    Span* span = fetch_from_page_cache(class_index);
    if (span == nullptr) {
        return nullptr;  // 获取失败
    }

    // 把Span加入SpanList
    span_list.push_front(span);

    // 取出第一个对象返回
    void* obj = span->free_list_;
    span->free_list_ = *reinterpret_cast<void**>(span->free_list_);
    span->free_count_--;

    return obj;
}

void CentralCache::deallocate(void* obj, size_t class_index) {
    MutexGuard lock(locks_[class_index]);

    // 计算对象所在的页号（假设每页4KB）
    size_t page_size = getpagesize();
    uint64_t page_id = reinterpret_cast<uint64_t>(obj) / page_size;

    // 找到对象所在的Span
    // 注意：这里需要遍历查找，实际实现中可以用更高效的方式
    Span* span = nullptr;
    for (auto& span_obj : span_lists_[class_index]) {
        if (span_obj.page_id_ <= page_id &&
            span_obj.page_id_ + span_obj.num_pages_ > page_id) {
            span = &span_obj;
            break;
        }
    }

    if (span == nullptr) {
        return;  // 没找到，不应该发生
    }

    // 把对象放回Span的空闲链表（头插法）
    *reinterpret_cast<void**>(obj) = span->free_list_;
    span->free_list_ = obj;
    span->free_count_++;

    // 如果Span全部空闲，归还给PageCache
    // 判断条件：Span的页数 * 每页大小 == 这个SizeClass的大小 * 空闲对象数
    size_t class_size = SizeClass::get_size(class_index);
    size_t span_total_objects = (span->num_pages_ * page_size) / class_size;

    if (span->free_count_ == span_total_objects) {
        // 全部空闲，从SpanList移除
        span_lists_[class_index].remove(span);

        // 归还给PageCache
        span->free_list_ = nullptr;
        span->free_count_ = 0;
        PageCache::get_instance().free_span(span);
    }
}

Span* CentralCache::fetch_from_page_cache(size_t class_index) {
    // 根据SizeClass大小计算需要多少页
    size_t class_size = SizeClass::get_size(class_index);
    size_t page_size = getpagesize();

    // 计算一个Span能切出多少个小对象
    // 为了效率，一次从PageCache获取较大的Span
    size_t num_pages = (class_size * 256 + page_size - 1) / page_size;
    if (num_pages < 1) num_pages = 1;

    // 向PageCache申请
    Span* span = PageCache::get_instance().allocate_span(num_pages);
    if (span == nullptr) {
        return nullptr;
    }

    // 初始化Span
    span->size_class_ = class_index;
    span->free_count_ = 0;
    span->free_list_ = nullptr;

    // 把Span的内存切分成固定大小的小块
    size_t total_size = span->num_pages_ * page_size;
    void* ptr = reinterpret_cast<void*>(span->page_id_ * page_size);

    // 切分成class_size大小的小块
    size_t offset = 0;
    while (offset + class_size <= total_size) {
        void* obj = reinterpret_cast<void*>(reinterpret_cast<char*>(ptr) + offset);
        *reinterpret_cast<void**>(obj) = span->free_list_;
        span->free_list_ = obj;
        span->free_count_++;
        offset += class_size;
    }

    return span;
}

} // namespace cc_server
