#ifndef CONCURRENTCACHE_BASE_FORMAT_H
#define CONCURRENTCACHE_BASE_FORMAT_H

/**
 * @file format.h
 * @brief 格式化工具类
 *
 * 为什么需要这个类？
 * - 统一管理所有格式化逻辑，避免在 Logger 里散落
 * - 时间戳、线程ID等格式化可以复用
 * - 以后加新格式化需求（如十六进制、日期等）只需要改这里
 */

#include <string>
#include <cstdarg>

namespace cc_server {

    /**
     * @brief Format 类 - 静态格式化工具
     *
     * 设计原则：所有方法都是 static，不需要实例化
     * 为什么？格式化是纯函数，没有共享状态，直接调用更方便
     */
    class Format {
    public:
        /**
         * @brief 可变参数格式化
         * @param fmt  格式化字符串（和 printf 一样）
         * @param ...  可变参数
         * @return     格式化后的字符串
         *
         * 为什么需要这个？
         * - vsnprintf 是 C 函数，返回 char*，我们需要 std::string
         * - 这里封装了一层，更方便 C++ 使用
         *
         * 示例：Format::format("hello %s, age=%d", "tom", 25)
         * 返回：  "hello tom, age=25"
         */
        static std::string format(const char* fmt, ...);

        /**
         * @brief 获取当前时间戳（毫秒精度）
         * @return 格式化的字符串，如 "2026-04-24 15:30:00.123"
         *
         * 为什么需要毫秒精度？
         * - 精确定位问题发生的时间点
         * - 多线程环境下，毫秒级时间戳能帮助分析并发问题
         *
         * 时间精度说明：
         * - 秒级：2026-04-24 15:30:00
         * - 毫秒级：2026-04-24 15:30:00.123  ← 我们用这个
         */
        static std::string timestamp();

        /**
         * @brief 获取当前线程ID
         * @return 线程ID字符串
         *
         * 为什么需要线程ID？
         * - 多线程环境下，日志可能交错输出
         * - 有线程ID才能区分某条日志是哪个线程打印的
         *
         * 示例输出：[12345] 或 [0x7f4a3c2e1234]
         */
        static std::string threadId();
    };

} // namespace cc_server
#endif
