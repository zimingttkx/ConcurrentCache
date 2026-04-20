#ifndef CONCURRENTCACHE_BASE_LOG_H
#define CONCURRENTCACHE_BASE_LOG_H

#include <iostream>
#include <string>
#include <ctime>
#include <string.h>

namespace cc_server {
    // 日志级别
    enum class LogLevel {
        DEBUG,
        INFO,
        WARN,
        ERROR
    };

    /**
     * @brief Logger类：全局日志记录器
     *
     * 协作关系：
     * - 被所有模块调用，记录程序运行日志
     * - 使用单例模式，全局唯一实例
     * - 通过宏 LOG_DEBUG/LOG_INFO/LOG_WARN/LOG_ERROR 简化调用
     */
    class Logger {
    private:
        Logger();
        LogLevel level_;

    public:
        static Logger& getInstance();
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // 设置日志级别
        void setLevel(LogLevel level);
        // 记录日志消息
        void log(LogLevel level, const std::string& message);
    };

    // 日志宏：LOG_DEBUG(fmt, ...) / LOG_INFO(fmt, ...) / LOG_WARN(fmt, ...) / LOG_ERROR(fmt, ...)
#define LOG_DEBUG(fmt, ...) cc_server::Logger::getInstance().log(cc_server::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  cc_server::Logger::getInstance().log(cc_server::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  cc_server::Logger::getInstance().log(cc_server::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) cc_server::Logger::getInstance().log(cc_server::LogLevel::ERROR, fmt, ##__VA_ARGS__)

}