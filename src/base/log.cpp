#include "log.h"

namespace cc_server {
    // 构造函数：初始化日志级别为 INFO
    Logger::Logger() : level_(LogLevel::INFO) {}

    // 获取单例实例
    Logger& Logger::getInstance() {
        static Logger instance;
        return instance;
    }

    // 设置日志级别
    void Logger::setLevel(LogLevel level) {
        level_ = level;
    }

    // 记录日志
    // - 如果日志级别低于当前级别，直接返回
    // - 获取当前时间并格式化为字符串
    // - 根据级别打印不同颜色的前缀
    // - 输出时间戳和消息内容
    void Logger::log(LogLevel level, const std::string& message) {
        if (level < level_) return;

        time_t now = time(nullptr);
        char* dt = ctime(&now);
        if (dt[strlen(dt) - 1] == '\n') dt[strlen(dt) - 1] = '\0';

        switch (level) {
            case LogLevel::DEBUG:
                std::cout << "\033[34m[DEBUG] ";
                break;

            case LogLevel::INFO:
                std::cout << "\033[32m[INFO] ";
                break;

            case LogLevel::WARN:
                std::cout << "\033[33m[WARN] ";
                break;
            case LogLevel::ERROR:
                std::cout << "\033[31m[ERROR] ";
                break;
        }
        std::cout << dt << " " << message << "\033[0m" << std::endl;
    }
}