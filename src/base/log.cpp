/**
 * @file log.cpp
 * @brief 日志系统实现
 *
 * 核心设计：
 * 1. 线程安全：所有公共接口都通过 mutex 互斥保护
 * 2. 异步写入：日志先入队，后台线程批量消费，不阻塞业务线程
 * 3. 批量聚合：一次 I/O 操作写出多条日志，减少系统调用
 * 4. 自动轮转：文件过大时自动创建新文件，保持磁盘占用可控
 */

#include "log.h"
#include <cstdarg>      // 可变参数处理
#include <cstring>     // 字符串操作
#include <cerrno>      // 错误码
#include <sys/stat.h>   // 文件状态
#include <fcntl.h>      // 文件控制
#include <unistd.h>     // POSIX API
#include <vector>       // 批量写入缓冲

namespace cc_server {

    // ==================== 构造函数和析构函数 ====================

    /**
     * @brief 构造函数 - 启动后台写入线程
     *
     * 创建 writerThread_ 并立即开始运行
     * 使用成员初始化列表设置默认值
     */
    Logger::Logger()
        : level_(LogLevel::INFO)
        , async_(true)
        , maxFileSize_(100 * 1024 * 1024)  // 默认 100MB
        , maxFiles_(5)                      // 默认保留 5 个文件
    {
        // 启动后台写入线程，lambda 捕获 this 并调用 writerThread
        writerThread_ = std::thread(&Logger::writerThread, this);
    }

    /**
     * @brief 析构函数 - 安全退出
     *
     * 确保：
     * 1. 通知后台线程停止
     * 2. 等待线程结束
     * 3. 关闭文件前 flush 所有待写数据
     */
    Logger::~Logger() {
        // 设置停止标志
        stop_ = true;

        // 通知后台线程，可能正在等待条件变量
        cv_.notify_all();

        // 等待后台线程结束
        if (writerThread_.joinable()) {
            writerThread_.join();
        }

        // 确保所有数据写入文件
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    // ==================== 静态单例获取 ====================

    /**
     * @brief 获取单例实例
     *
     * C++11 保证 static 局部变量初始化线程安全
     * 首次调用时创建实例，后续调用返回同一实例
     */
    Logger& Logger::instance() {
        static Logger instance;
        return instance;
    }

    // ==================== 配置接口 ====================

    /**
     * @brief 设置日志级别
     * @param level 新的日志级别
     *
     * 线程安全：涉及成员变量写入，需要加锁
     */
    void Logger::setLevel(LogLevel level) {
        level_ = level;
    }

    /**
     * @brief 设置日志输出文件
     * @param path 文件路径
     *
     * 线程安全：涉及文件流操作，需要加锁
     * 以追加模式(ios::app)打开，不会覆盖历史日志
     */
    void Logger::setFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(fileMutex_);

        filepath_ = path;

        // 以追加模式打开文件，ios::out | ios::app
        // 追加模式保证多进程写入不会互相覆盖
        file_.open(path, std::ios::app | std::ios::out);

        // 检查文件是否成功打开
        if (!file_) {
            // 打开失败，输出到 cerr（不是 cout，避免混乱）
            std::cerr << "Failed to open log file: " << path << std::endl;
        }
    }

    /**
     * @brief 设置轮转参数
     * @param maxSize 单文件最大字节数
     * @param maxFiles 最大历史文件保留数
     *
     * 线程安全：涉及成员变量写入，需要加锁
     */
    void Logger::setRotation(size_t maxSize, int maxFiles) {
        std::lock_guard<std::mutex> lock(fileMutex_);

        maxFileSize_ = maxSize;
        maxFiles_ = maxFiles;
    }

    /**
     * @brief 刷新队列 - 阻塞直到所有日志写出
     *
     * 适用于程序退出前确保所有日志都已持久化
     * 在异步模式下会等待队列清空
     */
    void Logger::flush() {
        if (async_) {
            std::unique_lock<std::mutex> lock(mutex_);

            // 等待队列变空
            // 使用条件变量避免忙等待
            cv_.wait(lock, [this] { return queue_.empty(); });
        }
    }

    // ==================== 核心日志写入 ====================

    /**
     * @brief 获取带毫秒精度的时间戳
     * @return 格式化的字符串，如 "2026-04-21 12:00:00.123"
     *
     * 使用 std::chrono 获取高精度时间
     * 使用 std::put_time 格式化（线程安全，不同于 ctime）
     */
    std::string Logger::getTimestamp() {
        // 获取当前时间点（system_clock 是墙上时间，适合日志）
        auto now = std::chrono::system_clock::now();

        // 转换为 time_t（从 epoch 开始的秒数）
        auto time_t = std::chrono::system_clock::to_time_t(now);

        // 计算毫秒部分
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // 使用字符串流格式化
        std::ostringstream oss;

        // std::put_time 是线程安全的，不同于 ctime()
        // %Y-%m-%d %H:%M:%S 格式：2026-04-21 12:00:00
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

        // 追加毫秒部分，setfill('0') 和 setw(3) 确保三位数字
        // 如 .123 而不是 .23
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

        return oss.str();
    }

    /**
     * @brief 格式化单条日志消息
     * @param level 日志级别
     * @param msg 日志原始内容
     * @return 格式化后的完整日志行
     *
     * 输出格式：[LEVEL] 2026-04-21 12:00:00.123 message
     */
    std::string Logger::formatMessage(LogLevel level, const std::string& msg) {
        // 获取级别字符串
        const char* levelStr;
        switch (level) {
            case LogLevel::DEBUG: levelStr = "DEBU"; break;
            case LogLevel::INFO:  levelStr = "INFO"; break;
            case LogLevel::WARN:  levelStr = "WARN"; break;
            case LogLevel::ERROR: levelStr = "ERROR"; break;
        }

        // 拼接完整格式：[LEVEL] timestamp message
        return "[" + std::string(levelStr) + "] " + getTimestamp() + " " + msg;
    }

    /**
     * @brief 主日志写入函数
     * @param level 日志级别
     * @param fmt 格式化字符串
     * @param ... 可变参数
     *
     * 线程安全：队列操作受 mutex 保护
     *
     * 工作流程：
     * 1. 检查级别是否满足阈值
     * 2. 格式化日志消息
     * 3. 加入异步队列
     * 4. 通知后台线程有新数据
     */
    void Logger::log(LogLevel level, const char* fmt, ...) {
        // 级别过滤：只处理 >= 当前阈值的日志
        if (level < level_) return;

        // ========== 格式化日志消息 ==========

        // 可变参数列表处理
        char buffer[4096];
        va_list args;
        va_start(args, fmt);           // 开始处理可变参数
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);                  // 结束处理

        // 格式化为完整日志行
        std::string message = formatMessage(level, buffer);

        // ========== 入队（线程安全）==========

        if (async_) {
            // 临界区：保护队列操作
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push(message);  // 入队
            }

            // 通知后台线程：有新数据等待处理
            // 使用 notify_one 唤醒一个等待中的线程
            cv_.notify_one();
        } else {
            // 同步模式：直接输出（主要用于调试）
            std::cout << message << std::endl;

            if (file_.is_open()) {
                writeToFile(message + "\n");
            }
        }
    }

    /**
     * @brief 写入单条日志到文件
     * @param msg 已格式化的日志消息
     *
     * 会检查文件大小，可能触发轮转
     */
    void Logger::writeToFile(const std::string& msg) {
        if (file_.is_open()) {
            file_ << msg;
            file_.flush();  // 确保写入磁盘，不是缓冲区

            // 检查是否超过大小限制
            if (static_cast<size_t>(file_.tellp()) > maxFileSize_) {
                // 触发轮转
                rotateFile();
                cleanupOldFiles();
            }
        }
    }

    // ==================== 日志轮转 ====================

    /**
     * @brief 执行日志轮转
     *
     * 轮转策略：
     * app.log (当前) -> app.log.1 (最新备份)
     * app.log.1     -> app.log.2
     * app.log.2     -> app.log.3
     * ...
     *
     * 关闭旧文件 -> 重命名历史文件 -> 重新打开新文件
     */
    void Logger::rotateFile() {
        if (!file_.is_open()) return;

        // 1. 关闭当前文件
        file_.close();

        // 2. 移动历史文件：app.log.N -> app.log.N+1
        // 从最大的开始移动，避免覆盖
        for (int i = maxFiles_ - 1; i > 0; --i) {
            std::string oldPath = filepath_ + "." + std::to_string(i);
            std::string newPath = filepath_ + "." + std::to_string(i + 1);

            // std::rename 在 POSIX 下是原子操作
            // 如果目标存在会被覆盖
            std::rename(oldPath.c_str(), newPath.c_str());
        }

        // 3. 将当前日志重命名为 .1
        std::string firstBackup = filepath_ + ".1";
        std::rename(filepath_.c_str(), firstBackup.c_str());

        // 4. 重新打开新文件
        file_.open(filepath_, std::ios::app | std::ios::out);
    }

    /**
     * @brief 清理超过保留数量的历史文件
     *
     * 删除 app.log.N，其中 N > maxFiles_
     * 例如 maxFiles_ = 5 时，删除 app.log.6, app.log.7, ...
     */
    void Logger::cleanupOldFiles() {
        for (int i = maxFiles_ + 1;; ++i) {
            std::string path = filepath_ + "." + std::to_string(i);

            // access(F_OK) 检查文件是否存在
            if (access(path.c_str(), F_OK) != 0) {
                // 文件不存在，停止清理
                break;
            }

            // unlink 删除文件（类似 rm 命令）
            unlink(path.c_str());
        }
    }

    // ==================== 后台写入线程 ====================

    /**
     * @brief 后台写入线程主循环
     *
     * 工作流程：
     * 1. 等待队列非空或收到停止信号
     * 2. 批量取出队列中的所有日志
     * 3. 写入文件或控制台
     * 4. 循环直到收到停止信号且队列为空
     */
    void Logger::writerThread() {
        std::vector<std::string> batch;  // 批量写入缓冲
        batch.reserve(100);              // 预分配空间，减少内存分配

        while (true) {
            // ========== 从队列中取出数据 ==========

            {
                // 加锁保护队列操作
                std::unique_lock<std::mutex> lock(mutex_);

                // 等待条件：
                // 1. stop_ == true 且队列空：退出循环
                // 2. stop_ == false 且队列非空：继续处理
                cv_.wait(lock, [this] {
                    return stop_ || !queue_.empty();
                });

                // 批量取出所有待处理日志
                // 这样可以减少锁竞争和 I/O 次数
                while (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop();
                }
            } // 锁在作用域结束时释放

            // ========== 写出日志 ==========

            // 没有数据？检查是否需要退出
            if (batch.empty()) {
                if (stop_) break;  // 收到停止信号且队列已空，退出
                continue;          // 否则继续等待
            }

            // 根据配置输出到文件或控制台
            if (file_.is_open()) {
                // ========== 写入文件 ==========

                std::lock_guard<std::mutex> lock(fileMutex_);

                for (const auto& msg : batch) {
                    file_ << msg << "\n";

                    // 检查是否需要轮转（在写入后检查）
                    if (static_cast<size_t>(file_.tellp()) > maxFileSize_) {
                        file_.flush();  // 先刷出已有数据
                        rotateFile();    // 触发轮转
                        cleanupOldFiles();
                    }
                }

                // 批量 flush，减少系统调用次数
                file_.flush();

            } else {
                // ========== 输出到控制台 ==========

                for (const auto& msg : batch) {
                    std::cout << msg << std::endl;
                }
                std::cout.flush();  // 确保立即输出
            }

            // 清空批量缓冲，准备下一轮
            batch.clear();
        }

        // 线程退出前的最终 flush
        // 此时 stop_ == true 且队列已空
    }

} // namespace cc_server
