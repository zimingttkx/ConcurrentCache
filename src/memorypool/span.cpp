//
// span.cpp
// Span 和 SpanList 实现
//

#include "span.h"

namespace cc_server {

SpanList::SpanList() {
    head_ = new Span();  // 哨兵节点，简化边界处理
    head_->next_ = head_;
    head_->prev_ = head_;
}

SpanList::~SpanList() {
    // 删除所有非哨兵节点的Span
    Span* cur = head_->next_;
    while (cur != head_) {
        Span* next = cur->next_;
        delete cur;
        cur = next;
    }
    delete head_;  // 删除哨兵节点
}

void SpanList::push_front(Span* span) {
    span->next_ = head_->next_;
    span->prev_ = head_;
    head_->next_->prev_ = span;
    head_->next_ = span;
}

void SpanList::push_back(Span* span) {
    span->next_ = head_;
    span->prev_ = head_->prev_;
    head_->prev_->next_ = span;
    head_->prev_ = span;
}

void SpanList::remove(Span* span) {
    span->prev_->next_ = span->next_;
    span->next_->prev_ = span->prev_;
    // 断开span的指针，帮助调试
    span->next_ = nullptr;
    span->prev_ = nullptr;
}

size_t SpanList::size() const {
    size_t count = 0;
    for (Span* cur = head_->next_; cur != head_; cur = cur->next_) {
        ++count;
    }
    return count;
}

} // namespace cc_server
