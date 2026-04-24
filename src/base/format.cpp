#include "format.h"
#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>

namespace cc_server {

/**
 * @brief 格式化函数实现
 *
 * 实现思路：
 * 1. 分配一个固定大小的缓冲区（4KB，够用）
 * 2. 用 vsnprintf 把格式化结果写到缓冲区
 * 3. 返回 string 形式的结果
 *
 * 为什么用 vsnprintf 而不是 vsprintf？
 * - vsnprintf 会限制写入字节数，避免缓冲区溢出
 * - sizeof(buffer) 就是我们的安全边界
 */
std::string Format::format(const char* fmt, ...) {
    // 缓冲区，4KB 够存绝大多数日志消息了
    char buffer[4096];

    // va_list 是 C 的可变参数列表
    // 类似于一个指针，指向可变参数的起始位置
    va_list args;

    // 开始处理可变参数
    // 第一个参数是 fmt 本身，va_start 知道从哪里开始是真正的参数
    va_start(args, fmt);

    // vsnprintf 是核心：
    // - buffer: 输出缓冲区
    // - sizeof(buffer): 最大写入字节数（安全边界）
    // - fmt: 格式化字符串
    // - args: 可变参数列表
    // 返回值是实际写入的字符数（不包括末尾的\0）
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    // 结束可变参数处理
    // 必须调用，否则行为未定义
    va_end(args);

    // 返回 string，避免返回指针还要考虑生命周期
    return std::string(buffer);
}

/**
 * @brief 获取时间戳实现
 *
 * 实现思路（分三步）：
 * 1. 获取"当前时间点"（一个精确到毫秒的时间值）
 * 2. 把时间点转成"从1970年到现在的秒数"（time_t）
 * 3. 单独计算毫秒部分
 * 4. 拼接成格式化字符串
 *
 * 为什么不用 ctime()？
 * - ctime() 返回的是静态缓冲区，线程不安全
 * - std::put_time 和 std::ostringstream 是线程安全的
 */
std::string Format::timestamp() {
    // 第一步：获取当前时间点
    // system_clock::now() 返回自 epoch（1970-01-01）以来的时间
    auto now = std::chrono::system_clock::now();

    // 第二步：转成 time_t（从 epoch 开始的"秒数"）
    // to_time_t 返回的是 time_t 类型，就是"从1970年到现在的秒数"
    auto time_t = std::chrono::system_clock::to_time_t(now);

    // 第三步：计算毫秒
    // duration_cast 把时间转成指定单位（毫秒）
    // % 1000 取余数，得到毫秒部分（0-999）
    // epoch_ms 例子：1500000000123，epoch_ms % 1000 = 123
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    // 第四步：格式化输出
    std::ostringstream oss;

    // std::put_time：类似 strftime，但线程安全
    // %Y-%m-%d %H:%M:%S 输出 "2026-04-24 15:30:00"
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

    // 追加毫秒部分
    // setfill('0') 和 setw(3)：保证3位数字，123→".123" 而不是 ".23"
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

/**
 * @brief 获取线程ID实现
 *
 * 为什么获取线程ID？
 * - 多线程程序里，日志会交错输出
 * - 比如线程A的日志可能插在线程B的日志中间
 * - 有了线程ID，就能过滤出某个线程的所有日志
 *
 * std::this_thread::get_id() 返回的是 std::thread::id 类型
 * 直接输出不好看，所以用 ostringstream 转成字符串
 */
std::string Format::threadId() {
    std::ostringstream oss;

    // << 操作符被重载了，直接输出 thread::id
    // 输出类似：12345 或 0x7f4a3c2e1234（取决于实现）
    oss << std::this_thread::get_id();

    return oss.str();
}

} // namespace cc_server
