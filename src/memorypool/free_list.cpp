//
// free_list.cpp
// FreeList 空闲链表实现
//

#include "free_list.h"

namespace cc_server {

void FreeList::push(void* obj) {
    // 把obj的前8个字节（64位系统）转换成指针，作为next指针
    *reinterpret_cast<void**>(obj) = head_;
    head_ = obj;
    ++size_;
}

void* FreeList::pop() {
    if (head_ == nullptr) {
        return nullptr;
    }

    void* obj = head_;
    // 把当前头节点的"下一个"作为新的头节点
    head_ = *reinterpret_cast<void**>(head_);
    --size_;

    return obj;
}

size_t FreeList::pop_batch(void** objs, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n && head_ != nullptr; ++i) {
        objs[i] = pop();
        ++count;
    }
    return count;
}

void FreeList::push_batch(void** objs, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        push(objs[i]);
    }
}

} // namespace cc_server
