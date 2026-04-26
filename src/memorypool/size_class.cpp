//
// size_class.cpp
// SizeClass 大小分类实现
//

#include "size_class.h"

namespace cc_server {

size_t SizeClass::get_index(size_t size) {
    // 如果请求超过256KB，返回-1（用malloc处理）
    if (size > kSizeClasses[kNumClasses - 1]) {
        return static_cast<size_t>(-1);
    }

    // 线性搜索找到第一个 >= size 的 SizeClass
    // 因为kSizeClasses是递增的，找到的第一个就是最小的合适大小
    for (size_t i = 0; i < kNumClasses; ++i) {
        if (kSizeClasses[i] >= size) {
            return i;
        }
    }

    // 理论上不会走到这里（前面已经检查过超过256KB的情况）
    return kNumClasses - 1;
}

size_t SizeClass::get_size(size_t index) {
    return kSizeClasses[index];
}

size_t SizeClass::round_up(size_t size) {
    if (size > kSizeClasses[kNumClasses - 1]) {
        return size;  // 超过256KB的不处理
    }
    return kSizeClasses[get_index(size)];
}

} // namespace cc_server
