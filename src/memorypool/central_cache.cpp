//
// central_cache.cpp
// CentralCache 中心缓存实现
//

#include "central_cache.h"
#include "page_cache.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  // getpagesize
#endif

namespace cc_server {

// 获取系统页面大小（跨平台）
static size_t get_page_size() {
#ifdef _WIN32
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwPageSize;
#else
    return static_cast<size_t>(getpagesize());
#endif
}

CentralCache& CentralCache::get_instance() {
    static CentralCache instance;
    return instance;
}

void* CentralCache::allocate(size_t class_index) {
    // 获取对应SizeClass的锁（细粒度锁，不同SizeClass可以并行）
    MutexGuard lock(locks_[class_index]);

    SpanList& span_list = span_lists_[class_index];

    // 步骤1：从链表里找一个还有空闲对象的 Span。
    // front 是最近切出来的，多数时候就是它；但要扫全表，因为被取空过的 Span
    // 之后还会有对象还回来。
    //
    // 关键：取空的 Span 绝对不能从链表里摘掉。deallocate 只能靠"页号落在
    // [page_id_, page_id_+num_pages_) 里"来认对象属于哪个 Span，摘掉就等于把
    // 这批还在使用的地址从可识别集合里删了 —— 归还时找不到归属，旧代码接着
    // 就走出链表末尾去读野内存（CI 上 ConcurrencyTests 的 SEGFAULT，以及 ASan
    // 报的 heap-buffer-overflow）。
    for (auto& span_obj : span_list) {
        if (span_obj.free_count_ > 0) {
            void* obj = span_obj.free_list_;
            span_obj.free_list_ = *reinterpret_cast<void**>(span_obj.free_list_);
            span_obj.free_count_--;

            return obj;
        }
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
    size_t page_size = get_page_size();
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
    // 判断条件：free_count_ 达到实际切分出的对象总数（total_objects_）
    // 修复 P1/Critical：此前用 num_pages*page_size/class_size 重算阈值，
    // 对不能整除的 size class（384B/8B 等）阈值永远达不到，Span 永不留还 → 泄漏。
    // 现在在切分时把实际 carved 数存入 span->total_objects_，直接比较即可。
    if (span->free_count_ == span->total_objects_) {
        // 全部空闲，从SpanList移除
        span_lists_[class_index].remove(span);

        // 归还给PageCache
        span->free_list_ = nullptr;
        span->free_count_ = 0;
        span->total_objects_ = 0;
        PageCache::get_instance().free_span(span);
    }
}

Span* CentralCache::fetch_from_page_cache(size_t class_index) {
    // 根据SizeClass大小计算需要多少页
    size_t class_size = SizeClass::get_size(class_index);
    size_t page_size = get_page_size();

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
    // 记录实际切分出的对象总数，归还判定以它为准（修复 P1-1：此前用
    // num_pages*page_size/class_size 重算，对不能整除的 size class 永不达标导致泄漏）
    span->total_objects_ = span->free_count_;

    return span;
}

} // namespace cc_server
