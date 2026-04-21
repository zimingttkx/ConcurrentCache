#ifndef CONCURRENTCACHE_BASE_LOG_H
#define CONCURRENTCACHE_BASE_LOG_H

/**
 * @file log.h
 * @brief 日志系统头文件
 *
 * 设计目标：
 * - 线程安全：多线程并发调用不会导致数据竞争
 * - 异步写入：日志操作不阻塞业务线程
 * - 日志轮转：自动管理日志文件大小，避免磁盘占满
 * - 毫秒精度：时间戳精确到毫秒，便于调试
 *
 * 架构概览：
 *   Logger (对外接口，单例)
 *       │
 *       └─▶ 异步队列 (线程安全队列)
 *               │
 *               └─▶ 后台写入线程 (批量消费)
 *                       │
 *                       ├─▶ 控制台输出
 *                       └─▶ 文件输出 (支持轮转)
 */

#include <iostream>      // 标准输出
#include <fstream>       // 文件输出
#include <string>        // 字符串处理
#include <mutex>         // 互斥锁，保证线程安全
#include <thread>        // 后台写入线程
#include <queue>         // 异步队列
#include <condition_variable>  // 条件变量，线程通信
#include <atomic>        // 原子变量，无锁标志位
#include <chrono>        // 时间处理，毫秒精度
#include <iomanip>       // 时间格式化
#include <memory>        // 智能指针
#include <sstream>       // 字符串流

namespace cc_server {

    /**
     * @brief 日志级别枚举
     *
     * 级别从低到高：DEBUG < INFO < WARN < ERROR
     * 只输出级别 >= 当前设置级别的日志
     */
    enum class LogLevel {
        DEBUG,  ///< 调试信息，默认不输出
        INFO,   ///< 一般信息
        WARN,   ///< 警告信息
        ERROR   ///< 错误信息
    };

    /**
     * @brief Logger 类 - 全局日志记录器
     *
     * 使用单例模式，保证全局只有一个日志实例
     *
     * 核心机制：
     * 1. 线程安全：所有公共接口都使用 mutex 保护
     * 2. 异步写入：日志先入队，后台线程批量写出
     * 3. 自动轮转：文件超过阈值时自动创建新文件
     *
     * 使用示例：
     *   Logger::instance().setLevel(LogLevel::INFO);
     *   Logger::instance().setFile("logs/app.log");
     *   LOG_INFO("server started on port %d", 8080);
     */
    class Logger {
    public:
        /**
         * @brief 析构函数
         *
         * 确保后台写入线程安全退出，并刷新所有待写入日志
         */
        ~Logger();

        /**
         * @brief 获取单例实例（线程安全）
         * @return Logger& 全局唯一实例
         *
         * 使用 C++11 static 局部变量保证线程安全初始化
         */
        static Logger& instance();

        /**
         * @brief 设置日志级别阈值
         * @param level 要设置的级别，只有 >= 此级别的日志才会输出
         *
         * 示例：setLevel(LogLevel::INFO) 会输出 INFO/WARN/ERROR，但忽略 DEBUG
         */
        void setLevel(LogLevel level);

        /**
         * @brief 设置日志输出文件
         * @param path 文件路径，如 "logs/app.log"
         *
         * 调用后日志会同时输出到控制台和文件
         * 文件以追加模式打开，不覆盖历史日志
         */
        void setFile(const std::string& path);

        /**
         * @brief 设置日志轮转参数
         * @param maxSize 单个日志文件最大大小（字节）
         * @param maxFiles 保留的最多历史文件数
         *
         * 示例：setRotation(100*1024*1024, 5)
         *   - 每个日志文件最大 100MB
         *   - 最多保留 5 个历史文件 (app.log.1 ~ app.log.5)
         */
        void setRotation(size_t maxSize, int maxFiles);

        /**
         * @brief 刷新日志队列（阻塞直到所有日志写出）
         *
         * 在程序退出前调用，确保所有日志都已写入文件
         */
        void flush();

        /**
         * @brief 记录日志（可变参数版本）
         * @param level 日志级别
         * @param fmt 格式化字符串（printf 风格）
         * @param ... 可变参数
         *
         * 内部会自动获取时间戳、格式化、线程安全入队
         */
        void log(LogLevel level, const char* fmt, ...);

        // 禁用拷贝构造和赋值，确保单例唯一性
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

    private:
        /**
         * @brief 私有构造函数
         *
         * 创建后台写入线程，开始异步消费日志队列
         */
        Logger();

        /**
         * @brief 后台写入线程主函数
         *
         * 循环从队列中取出日志，批量写入文件或控制台
         * 使用条件变量等待新日志，避免忙等待
         */
        void writerThread();

        /**
         * @brief 执行日志文件轮转
         *
         * 将当前日志文件重命名为 .1，然后创建新文件
         * 例如：app.log -> app.log.1
         */
        void rotateFile();

        /**
         * @brief 清理超过保留数量的历史文件
         *
         * 删除 app.log.N (N > maxFiles_) 的文件
         */
        void cleanupOldFiles();

        /**
         * @brief 获取带毫秒精度的时间戳字符串
         * @return 格式如 "2026-04-21 12:00:00.123"
         *
         * 使用 std::chrono 保证线程安全，不依赖 ctime() 的静态缓冲区
         */
        std::string getTimestamp();

        /**
         * @brief 格式化单条日志消息
         * @param level 日志级别
         * @param msg 日志内容
         * @return 格式化后的完整日志行
         *
         * 输出格式：[LEVEL] 2026-04-21 12:00:00.123 message
         */
        std::string formatMessage(LogLevel level, const std::string& msg);

        /**
         * @brief 写入单条日志到文件
         * @param msg 已格式化的日志消息
         *
         * 内部会检查是否需要触发轮转
         */
        void writeToFile(const std::string& msg);

        // ==================== 成员变量 ====================

        LogLevel level_;                    ///< 日志级别阈值
        bool async_;                        ///< 是否异步模式

        std::ofstream file_;                ///< 日志文件输出流
        std::string filepath_;              ///< 日志文件路径
        size_t maxFileSize_;                ///< 单文件最大大小（字节）
        int maxFiles_;                      ///< 保留的最大历史文件数

        std::thread writerThread_;          ///< 后台写入线程
        std::queue<std::string> queue_;    ///< 异步日志队列
        std::mutex mutex_;                  ///< 队列操作互斥锁
        std::condition_variable cv_;        ///< 队列非空条件变量
        std::atomic<bool> stop_{false};    ///< 线程停止标志

        std::mutex fileMutex_;              ///< 文件操作互斥锁（保护文件 I/O）
    };

    // ==================== 日志宏 ====================

    /**
     * @brief 日志宏 - 简化日志调用
     *
     * 使用可变参数宏，支持 printf 风格格式化
     *
     * 示例：
     *   LOG_DEBUG("connection fd=%d", fd);
     *   LOG_INFO("server started on port %d", 8080);
     *   LOG_ERROR("recv failed: %s", strerror(errno));
     */
#define LOG_DEBUG(fmt, ...) cc_server::Logger::instance().log(cc_server::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  cc_server::Logger::instance().log(cc_server::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  cc_server::Logger::instance().log(cc_server::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) cc_server::Logger::instance().log(cc_server::LogLevel::ERROR, fmt, ##__VA_ARGS__)

} // namespace cc_server
