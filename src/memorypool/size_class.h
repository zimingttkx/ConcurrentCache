//
// size_class.h
// 内存池大小分类 - 版本2核心组件
//
// 29个SizeClass：从8字节到262144字节（256KB）
// 申请时向上取整到最接近的SizeClass
//

#ifndef CONCURRENTCACHE_SIZE_CLASS_H
#define CONCURRENTCACHE_SIZE_CLASS_H

#include <cstddef>

namespace cc_server {

class SizeClass {
public:
    //
    // SizeClass 数组 - 固定大小档次（单位：字节）
    // 越小的档次越密集（因为小对象申请更频繁）
    //
    static constexpr size_t kSizeClasses[] = {
        8,      // 第0级：8字节
        16,     // 第1级：16字节
        32,     // 第2级：32字节
        48,     // 第3级：48字节
        64,     // 第4级：64字节
        96,     // 第5级：96字节
        128,    // 第6级：128字节
        192,    // 第7级：192字节
        256,    // 第8级：256字节
        384,    // 第9级：384字节
        512,    // 第10级：512字节
        768,    // 第11级：768字节
        1024,   // 第12级：1KB
        1536,   // 第13级：1.5KB
        2048,   // 第14级：2KB
        3072,   // 第15级：3KB
        4096,   // 第16级：4KB
        6144,   // 第17级：6KB
        8192,   // 第18级：8KB
        12288,  // 第19级：12KB
        16384,  // 第20级：16KB
        24576,  // 第21级：24KB
        32768,  // 第22级：32KB
        49152,  // 第23级：48KB
        65536,  // 第24级：64KB
        98304,  // 第25级：96KB
        131072, // 第26级：128KB
        196608, // 第27级：192KB
        262144  // 第28级：256KB
    };

    // SizeClass 的总数量
    static constexpr size_t kNumClasses = 29;

    //
    // 根据实际请求大小，找到最接近的SizeClass索引
    // @param size 实际请求的字节数
    // @return SizeClass索引（0 ~ kNumClasses-1），如果超过256KB返回-1
    //
    static size_t get_index(size_t size);

    //
    // 根据索引获取对应的大小
    // @param index SizeClass索引
    // @return 对应的字节数
    //
    static size_t get_size(size_t index);

    //
    // 将任意大小向上取整到最接近的SizeClass
    // @param size 原始大小
    // @return 向上取整后的大小，超过256KB返回原值
    //
    static size_t round_up(size_t size);
};

} // namespace cc_server

#endif // CONCURRENTCACHE_SIZE_CLASS_H
