//
// free_list.h
// 空闲链表 - 版本2核心组件
//
// 管理一组空闲内存对象的链表
// 分配时从链表取一个对象（pop），释放时放回链表（push）
// 头插头取，O(1)操作
//

#ifndef CONCURRENTCACHE_FREE_LIST_H
#define CONCURRENTCACHE_FREE_LIST_H

#include <cstddef>

namespace cc_server {

class FreeList {
public:
    // 构造函数
    FreeList() : head_(nullptr), size_(0) {}

    //
    // 插入一个对象到链表头部（头插法）
    // @param obj 要插入的对象指针
    //
    // 为什么用头插法？
    // - O(1)时间复杂度，不需要遍历
    // - 刚释放的对象更可能马上再用（局部性原理）
    //
    void push(void* obj);

    //
    // 从链表头部取出一个对象
    // @return 取出的对象指针，如果链表为空返回nullptr
    //
    void* pop();

    //
    // 批量取出多个对象
    // @param objs 数组，用于存放取出的对象指针
    // @param n 要取出的数量
    // @return 实际取出的数量
    //
    size_t pop_batch(void** objs, size_t n);

    //
    // 批量放回多个对象
    // @param objs 数组，包含要放回的对象指针
    // @param n 要放回的数量
    //
    void push_batch(void** objs, size_t n);

    // 获取当前链表中对象的数量
    [[nodiscard]] size_t size() const { return size_; }

    // 判断链表是否为空
    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    // 获取链表头节点（不弹出），用于调试
    [[nodiscard]] void* head() const { return head_; }

private:
    void* head_;      // 链表头指针
    size_t size_;     // 链表中的对象数量
};

} // namespace cc_server

#endif // CONCURRENTCACHE_FREE_LIST_H
