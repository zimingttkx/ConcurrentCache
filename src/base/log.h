#ifndef CONCURRENTCACHE_BASE_LOG_H
#define CONCURRENTCACHE_BASE_LOG_H

#include <iostream>
#include <string>
#include <ctime>
#include <string.h>

// 命名空间封装 防止全局命名污染
namespace cc_server {
    // 日志级别枚举
    enum class LogLevel {
        DEBUG,
        INFO,
        WARN,
        ERROR
    };

    // 日志类 使用单例模式 确保全局只有一个日志实例
    class Logger {
    private:
        Logger() : level_(LogLevel::INFO){};
        LogLevel level_;


    public:
        // 获取单例实例的静态方法
        static Logger& getInstance() {
            static Logger instance;
            return instance;
        }
        // 删除拷贝构造函数和赋值运算符
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // 设置日志级别
        void setLevel(LogLevel level) {
            level_ = level;
        }
        // 记录日志
        void log(LogLevel level, const std::string& message) {
            // 如果当前日志级别低于设定的级别 就不打印
            if (level < level_) return;

            // 获取当前时间
            time_t now = time(nullptr);
            char * dt = ctime(&now);
            // 去除ctime返回的换行符
            if (dt[strlen(dt) - 1] == "\n") dt[strlen(dt) - 1] = '\0';

            // 根据不同级别打印不同颜色
            switch (level) {
                case LogLevel::DEBUG:
                    std::cout << "\033[34m[DEBUG] "; // 蓝色
                    break;

                case LogLevel::INFO:
                    std::cout << "\033[32m[INFO] "; // 绿色
                    break;

                case LogLevel::WARN:
                    std::cout << "\033[33m[WARN] "; // 黄色
                    break;
                case LogLevel::ERROR:
                    std::cout << "\033[31m[ERROR] "; // 红色
                    break;
            }
            std::cout << dt << " " << message << "\033[0m" << std::endl;
        }
    };
    // 定义便捷宏，简化调用
    // 使用 ##__VA_ARGS__ 处理宏参数为空的情况
#define LOG_DEBUG(fmt, ...) cc_server::Logger::getInstance().log(cc_server::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  cc_server::Logger::getInstance().log(cc_server::LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  cc_server::Logger::getInstance().log(cc_server::LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) cc_server::Logger::getInstance().log(cc_server::LogLevel::ERROR, fmt, ##__VA_ARGS__)

}