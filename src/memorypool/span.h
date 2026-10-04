//
// span.h
// Span 和 SpanList - 版本2核心组件
//
// Span：管理一组连续内存页的结构
// SpanList：管理同一SizeClass的所有Span的双向链表（带哨兵节点）
//

#ifndef CONCURRENTCACHE_SPAN_H
#define CONCURRENTCACHE_SPAN_H

#include <cstdint>
#include <cstddef>

namespace cc_server {

// Span：内存跨距，管理连续内存页
// page_id_：起始页号（一页 = 4KB）
// num_pages_：这个Span包含多少页
struct Span {
    uint64_t page_id_;      // 起始页号
    size_t num_pages_;      // 页数
    size_t size_class_;     // 对应的SizeClass索引

    void* free_list_;       // 空闲小块链表
    size_t free_count_;     // 空闲小块的数量
    size_t total_objects_;  // 实际切分出的对象总数（用于归还判定，避免泄漏）

    Span* next_;            // 链表指针
    Span* prev_;

    Span() : page_id_(0), num_pages_(0), size_class_(0),
             free_list_(nullptr), free_count_(0), total_objects_(0),
             next_(nullptr), prev_(nullptr) {}
};

// SpanList：双向循环链表，管理同一SizeClass的所有Span
class SpanList {
public:
    SpanList();

    // 析构函数
    ~SpanList();

    // 禁用拷贝
    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    // 在链表头部插入一个Span
    void push_front(Span* span);

    // 在链表尾部插入一个Span
    void push_back(Span* span);

    // 从链表移除一个Span
    void remove(Span* span);

    // 判断链表是否为空（只有哨兵节点）
    [[nodiscard]] bool empty() const { return head_->next_ == head_; }

    // 获取链表第一个真实节点
    [[nodiscard]] Span* front() const { return head_->next_; }

    // 获取链表最后一个真实节点
    [[nodiscard]] Span* back() const { return head_->prev_; }

    // 获取链表大小（不包含哨兵节点）
    [[nodiscard]] size_t size() const;

    // 遍历链表。
    // begin()/end() 不能直接返回 Span*：那样 range-for 里的 ++it 就是指针自增
    // （访问的是 span+1 这块根本没分配过的内存），而不是走 next_。节点是逐个
    // new 出来的，彼此不连续，所以这种遍历读到的全是野指针 —— CentralCache::
    // deallocate 就因此既找不到正确的 Span，又会踩内存（ASan: heap-buffer-overflow
    // "READ of size 8, 0 bytes after 64-byte region"，region 正是 sizeof(Span)）。
    class Iterator {
    public:
        explicit Iterator(Span* pos) : pos_(pos) {}

        Span& operator*() const { return *pos_; }
        Span* operator->() const { return pos_; }

        Iterator& operator++() {
            pos_ = pos_->next_;
            return *this;
        }

        bool operator==(const Iterator& other) const { return pos_ == other.pos_; }
        bool operator!=(const Iterator& other) const { return pos_ != other.pos_; }

    private:
        Span* pos_;
    };

    Iterator begin() const { return Iterator(head_->next_); }
    Iterator end() const { return Iterator(head_); }

private:
    Span* head_;  // 哨兵节点，简化空链表判断
};

} // namespace cc_server

#endif // CONCURRENTCACHE_SPAN_H
