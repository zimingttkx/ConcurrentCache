#ifndef CONCURRENTCACHE_BASE_LOG_H
#define CONCURRENTCACHE_BASE_LOG_H

/**
 * @file log.h
 * @brief 日志系统头文件
 *
 * 版本2设计目标：
 * 1. Sink 抽象 - 可以灵活组合不同的输出目标（控制台、文件、网络等）
 * 2. 模块支持 - 日志按模块分类，便于定位问题
 * 3. 配置热加载 - 修改配置文件后日志级别自动生效
 * 4. 六级日志 - TRACE/DEBUG/INFO/WARN/ERROR/FATAL
 *
 * 架构图：
 *   Logger（单例，对外接口）
 *       │
 *       ├── 队列（线程安全）
 *       │
 *       └── 后台线程（批量消费）
 *               │
 *               ├── ConsoleSink（控制台）
 *               ├── FileSink（文件，支持轮转）
 *               └── 更多 Sink 可以随时添加...
 */

// ==================== 头文件引用 ====================
// 每一个 include 都有原因：

#include <iostream>           // std::cout，控制台输出
#include <fstream>            // std::ofstream，文件输出
#include <string>             // std::string，字符串
#include <mutex>              // std::mutex，互斥锁，保护共享数据
#include <thread>             // std::thread，后台写入线程
#include <queue>              // std::queue，异步日志队列
#include <condition_variable> // std::condition_variable，线程通信
#include <atomic>             // std::atomic，原子变量，无锁标志位
#include <chrono>             // std::chrono，时间处理（已移到Format）
#include <iomanip>            // std::setfill/std::setw，时间格式化
#include <memory>             // std::shared_ptr，智能指针，管理Sink生命周期
#include <sstream>            // std::ostringstream，字符串拼接
#include <vector>             // std::vector，存储多个Sink
#include <cstdarg>            // 可变参数，va_list/va_start/va_end
#include "config.h"
#include <cstdarg>

namespace cc_server {

// ==================== Sink 抽象层 ====================
/**
 * @brief Sink 基类 - 日志输出目标的抽象
 *
 * 为什么需要 Sink 抽象？
 * - 旧版本：日志输出写死在 Logger 里，要改输出方式得改 Logger 源码
 * - 新版本： Logger 只管把日志放到队列，输出方式由 Sink 决定
 * - 好处：可以同时写控制台和文件，可以以后加网络发送、syslog等
 *
 * 设计思路：
 * - 所有输出目标都实现 write() 和 flush() 两个方法
 * - Logger 不关心具体是哪种 Sink
 * - 新增输出方式只需要新建一个类，实现这两个方法即可
 */
class Sink {
public:
    // 虚析构函数 - 保证基类指针删除子类对象时正确调用子类析构
    virtual ~Sink() = default;

    /**
     * @brief 写一条日志
     * @param message 已经格式化好的日志消息
     *
     * 为什么参数是 string 而不是 char*？
     * - string 管理内存，不需要调用方担心长度和生命周期
     * - 可以直接追加、复制，比 char* 安全
     */
    virtual void write(const std::string& message) = 0;

    /**
     * @brief 刷新缓冲区
     *
     * 为什么需要 flush？
     * - write() 可能只是把数据写到缓冲区，没有真正写到磁盘/屏幕
     * - flush() 强制把缓冲区内容真正写出去
     * - 程序退出前必须调用，确保所有日志都输出
     */
    virtual void flush() = 0;
};

/**
 * @brief 控制台 Sink - 输出到 stdout
 *
 * 实现要点：
 * - 直接 << 到 cout endl 会自动 flush（带换行）
 * - 也可以用 printf，但 << 更 C++ 风格
 */
class ConsoleSink : public Sink {
public:
    /** @brief 写日志到控制台 */
    void write(const std::string& message) override;

    /** @brief 刷新控制台缓冲区 */
    void flush() override;
};

/**
 * @brief 文件 Sink - 输出到文件，支持自动轮转
 *
 * 为什么需要轮转？
 * - 日志文件会一直增长，不加限制会把磁盘撑满
 * - 轮转策略：app.log → app.log.1 → app.log.2 → ...
 * - 超过最大文件数就删除最老的
 *
 * 核心机制：
 * - 写入前检查文件大小
 * - 超过 maxSize 就触发 rotate()
 * - rotate() 会关闭旧文件、重命名、创建新文件
 */
class FileSink : public Sink {
public:
    /**
     * @brief 构造函数
     * @param filepath 文件路径，如 "./logs/app.log"
     * @param maxSize 单文件最大字节数
     * @param maxFiles 最多保留几个历史文件
     */
    FileSink(const std::string& filepath, size_t maxSize, int maxFiles);

    /** @brief 析构函数，确保关闭文件 */
    ~FileSink();

    void write(const std::string& message) override;
    void flush() override;

private:
    /**
     * @brief 执行日志文件轮转
     *
     * 轮转步骤：
     * 1. 关闭当前文件
     * 2. 把 app.log.1 改成 app.log.2（依次后移）
     * 3. 把 app.log 改成 app.log.1
     * 4. 创建新的 app.log
     *
     * 为什么用 std::rename？
     * - POSIX 系统下 rename 是原子操作
     * - 不会出现中间状态（如 app.log.tmp）
     */
    void rotate();

    /**
     * @brief 清理超过数量限制的历史文件
     *
     * 比如 maxFiles=5，就删除 app.log.6, app.log.7...
     * 保留 app.log.1 ~ app.log.5
     */
    void cleanup();

    std::string filepath_;      // 日志文件路径
    size_t maxSize_;            // 单文件最大字节数
    int maxFiles_;              // 最大历史文件数
    std::ofstream file_;        // 文件输出流
    std::mutex mutex_;          // 保护文件操作（多线程写入同一文件）;
};

// ==================== 日志级别 ====================
/**
 * @brief 日志级别枚举
 *
 * 级别从低到高：TRACE < DEBUG < INFO < WARN < ERROR < FATAL
 * 只输出级别 >= 当前设置的阈值
 *
 * 各级别使用场景：
 * - TRACE：函数入口/出口、变量值变化（最详细）
 * - DEBUG：调试信息，开发时用
 * - INFO：一般信息，如服务器启动、连接建立
 * - WARN：警告，不影响运行但需要注意（如连接超时）
 * - ERROR：错误，影响某个操作但服务器能继续
 * - FATAL：致命错误，程序无法继续运行
 */
enum class LogLevel {
    TRACE,  // 新增：最细粒度
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL   // 新增：致命错误
};

// ==================== Logger 主类 ====================
/**
 * @brief Logger 类 - 全局日志记录器（单例模式）
 *
 * 为什么用单例？
 * - 全局只需要一个 Logger 实例
 * - 所有模块都往同一个 Logger 写日志
 * - 方便统一管理日志输出
 *
 * 核心设计：
 * 1. 异步写入：日志先入队列，后台线程批量写出，不阻塞业务
 * 2. 多 Sink：可以同时输出到多个目标
 * 3. 模块化：每个日志都带模块名，便于过滤
 * 4. 热加载：实现 ConfigObserver，配置文件改了就自动更新级别
 */
class Logger : public ConfigObserver {
public:
    /**
     * @brief 析构函数
     *
     * 必须做的事情：
     * 1. 通知后台线程停止
     * 2. 等待线程结束（join）
     * 3. 刷新所有待写日志
     */
    ~Logger();

    /**
     * @brief 获取单例实例
     *
     * C++11 static 局部变量特性：
     * - 首次调用时构造，线程安全
     * - 后续调用直接返回已存在的实例
     * - 不需要加锁 double-checked locking
     */
    static Logger& instance();

    // ==================== 配置接口 ====================

    /**
     * @brief 设置日志级别阈值
     * @param level 级别阈值
     *
     * 示例：
     * - setLevel(INFO) → 输出 INFO/WARN/ERROR/FATAL，忽略 TRACE/DEBUG
     * - setLevel(ERROR) → 只输出 ERROR/FATAL
     */
    void setLevel(LogLevel level);


    void onConfigChange(const std::string& key, const std::string& value) override;

    /**
     * @brief 设置日志输出文件
     * @param path 文件路径
     *
     * 注意：
     * - 调用后日志会同时输出到控制台和文件
     * - 是追加模式，不会覆盖历史日志
     * - 内部会创建 FileSink 并添加到 sinks_ 列表
     */
    void setFile(const std::string& path);

    /**
     * @brief 设置日志轮转参数
     * @param maxSize 单文件最大字节数
     * @param maxFiles 最大历史文件保留数
     */
    void setRotation(size_t maxSize, int maxFiles);

    /**
     * @brief 刷新队列，阻塞直到所有日志写出
     *
     * 使用场景：
     * - 程序退出前调用
     * - 确保日志都写入文件后再退出
     * - 否则可能丢失还在队列里的日志
     */
    void flush();

    // ==================== 核心日志接口 ====================

    /**
     * @brief 记录带模块的日志（主要接口）
     * @param module 模块名，如 "NETWORK"、"CACHE"
     * @param level 日志级别
     * @param fmt 格式化字符串
     * @param ... 可变参数
     *
     * 工作流程：
     * 1. 检查级别是否满足阈值
     * 2. 用 vsnprintf 格式化消息
     * 3. 拼接完整格式：[LEVEL] timestamp [MODULE] [THREAD_ID] message
     * 4. 入队（加锁保证线程安全）
     * 5. 通知后台线程
     */
    void log(const char* module, LogLevel level, const char* fmt, ...);

    /**
     * @brief 记录不带模块的日志（兼容旧接口）
     * @param level 日志级别
     * @param fmt 格式化字符串
     * @param ... 可变参数
     *
     * 兼容原因：
     * - 旧代码可能用 LOG_INFO("message") 而不是 LOG_INFO(MODULE, "message")
     * - 这个重载使用默认模块名 "MAIN"
     */
    void log(LogLevel level, const char* fmt, ...);

    /**
     * @brief 配置变更通知（热加载实现）
     * @param key 配置项名
     * @param value 配置项新值
     *
     * 实现 ConfigObserver 接口：
     * - Config 检测到 log_level 变化时调用
     * - 把字符串转成 LogLevel 枚举
     * - 自动更新日志级别
     */

    // 禁用拷贝构造和赋值
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    /**
     * @brief 私有构造函数
     *
     * 初始化列表设置默认值：
     * - 级别：INFO
     * - 文件大小：100MB
     * - 历史文件数：5个
     *
     * 初始化时：
     * - 添加默认 ConsoleSink（控制台）
     * - 启动后台写入线程
     */
    Logger();

    /**
     * @brief 格式化日志消息
     * @param module 模块名
     * @param level 日志级别
     * @param msg 原始消息
     * @return 格式化后的完整日志行
     *
     * 输出格式：
     * [LEVEL] 2026-04-24 15:30:00.123 [MODULE] [THREAD_ID] message
     *
     * 示例：
     * [INFO] 2026-04-24 15:30:00.123 [NETWORK] [12345] connection accepted
     */
    std::string formatMessage(const char* module, LogLevel level, const std::string& msg);

    /**
     * @brief 后台写入线程主循环
     *
     * 工作流程：
     * 1. 等待队列非空或收到停止信号
     * 2. 批量取出队列中的所有日志
     * 3. 遍历所有 Sink，写入
     * 4. 刷新所有 Sink
     * 5. 循环直到收到停止信号且队列为空
     */
    void writerThread();

    // ==================== 成员变量 ====================

    LogLevel level_;                    // 日志级别阈值
    size_t maxFileSize_;                // 单文件最大字节数
    int maxFiles_;                      // 最大历史文件数

    std::thread writerThread_;          // 后台写入线程
    std::queue<std::string> queue_;     // 异步日志队列
    std::mutex mutex_;                  // 队列操作互斥锁
    std::condition_variable cv_;        // 条件变量，等待队列非空
    std::atomic<bool> stop_{false};    // 停止标志，原子操作

    std::vector<std::shared_ptr<Sink>> sinks_;  // Sink 列表，支持多个输出
    std::string defaultModule_;         // 默认模块名
};

// ==================== 日志宏（带模块） ====================
/**
 * @brief 日志宏 - 简化调用
 *
 * 为什么用宏而不是函数？
 * - 可以用可变参数 ...，灵活传递任意参数
 * - 可以自动把模块名转成字符串（#module）
 * - 调用简洁：LOG_INFO(NETWORK, "connected fd=%d", fd)
 *
 * #module 是啥？
 * - 这是预处理器的字符串化操作
 * - NETWORK → "NETWORK"
 * - 所以 LOG_INFO(NETWORK, ...) 等价于
 *   Logger::instance().log("NETWORK", LogLevel::INFO, ...)
 */
#define LOG_TRACE(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::TRACE, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::DEBUG, fmt, ##__VA_ARGS__)

#define LOG_INFO(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::INFO, fmt, ##__VA_ARGS__)

#define LOG_WARN(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::WARN, fmt, ##__VA_ARGS__)

#define LOG_ERROR(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::ERROR, fmt, ##__VA_ARGS__)

#define LOG_FATAL(module, fmt, ...) \
    cc_server::Logger::instance().log(#module, cc_server::LogLevel::FATAL, fmt, ##__VA_ARGS__)

} // namespace cc_server
#endif
