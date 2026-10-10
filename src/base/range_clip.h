#pragma once
// 列表/有序集合的 start..stop 索引裁剪（Redis 语义），唯一一份实现。
//
// 以前 LRANGE 与 ZRANGE 各写了一份：LRANGE 把裁剪后仍为负的 stop 夹到 0
// （`LRANGE k 0 -3` 在 2 元素表上返回下标 0），ZRANGE 直接返回空。同一个索引
// 表达式在两条命令上结果不同，而只有后者跟 Redis 一致 —— Redis 的做法是
// end<0 时返回空集合。抽成一个函数，两条命令不可能再分叉。
//
// 返回 false 表示结果区间为空（调用方直接返回空数组），
// 返回 true 时 out_start/out_stop 是已裁剪的闭区间下标（保证 out_start<=out_stop）。
#include <cstddef>

namespace cc_server {
namespace base {

inline bool clip_range(std::size_t size, long long start, long long stop,
                       long long& out_start, long long& out_stop) {
    if (size == 0) {
        return false;
    }
    const long long len = static_cast<long long>(size);
    long long actual_start = start < 0 ? len + start : start;
    long long actual_stop = stop < 0 ? len + stop : stop;

    if (actual_start < 0) {
        actual_start = 0;          // 起点越界按 Redis 处理：从头开始
    }
    if (actual_stop < 0) {
        return false;              // 终点仍在末尾之前：区间为空，不是"夹到 0"
    }
    if (actual_start >= len) {
        return false;
    }
    if (actual_stop >= len) {
        actual_stop = len - 1;
    }
    if (actual_start > actual_stop) {
        return false;
    }
    out_start = actual_start;
    out_stop = actual_stop;
    return true;
}

}  // namespace base
}  // namespace cc_server
