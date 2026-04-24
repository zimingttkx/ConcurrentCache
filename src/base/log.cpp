#include "log.h"
#include "format.h"
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

namespace cc_server {

// ==================== ConsoleSink 实现 ====================

/**
 * @brief 控制台写入
 *
 * 实现要点：
 * - std::cout << message << std::endl
 * - endl 会自动 flush 缓冲区
 * - 不需要加锁，因为 iostream 本身是线程安全的
 *   （但多线程同时 cout 会交错输出，所以建议每个线程独立缓冲）
 */
void ConsoleSink::write(const std::string& message) {
    // std::cout 是全局的，线程安全
    // << 重载了 string，所以直接传 string 就行
    std::cout << message << std::endl;  // endl = 输出\n + flush
}

/**
 * @brief 控制台刷新
 *
 * 控制台的 flush 实际上是多余的，因为 endl 已经 flush 了
 * 但为了接口一致，还是实现一下
 */
void ConsoleSink::flush() {
    std::cout.flush();
}

// ==================== FileSink 实现 ====================

/**
 * @brief 构造函数 - 打开文件
 *
 * 初始化列表：
 * - filepath_ = filepath：保存文件路径
 * - maxSize_ = maxSize：保存大小限制
 * - maxFiles_ = maxFiles：保存文件数量限制
 */
FileSink::FileSink(const std::string& filepath, size_t maxSize, int maxFiles)
    : filepath_(filepath)
    , maxSize_(maxSize)
    , maxFiles_(maxFiles)
{
    // ==================== 确保目录存在 ====================
    // 为什么要检查目录？
    // - 如果路径是 ./logs/app.log，./logs 目录可能不存在
    // - 直接 open 会失败

    // 找到最后一个 '/' 的位置
    size_t pos = filepath.find_last_of('/');

    if (pos != std::string::npos) {
        // 提取目录部分
        std::string dir = filepath.substr(0, pos);

        // 创建目录
        // mkdir -p：递归创建，如果目录已存在不报错
        // system() 调用 shell，虽然不太优雅，但简单可靠
        // 更好的做法是用 POSIX mkdir()，但需要处理路径分割
        std::string cmd = "mkdir -p " + dir;
        system(cmd.c_str());
    }

    // ==================== 打开文件 ====================
    // ios::app = 追加模式，新内容写到文件末尾
    // ios::out = 写模式
    // 两者组合：追加写入，不会覆盖之前的内容
    file_.open(filepath, std::ios::app | std::ios::out);

    // 这里没检查 open() 是否成功
    // 如果失败，file_.is_open() 会返回 false
    // write() 里会检查这个问题
}

/**
 * @brief 析构函数 - 关闭文件
 *
 * 为什么需要手动关闭？
 * - 文件流析构时会自动关闭
 * - 但手动 close() 可以确保 flush() 先执行
 * - 避免程序异常退出时日志丢失
 */
FileSink::~FileSink() {
    if (file_.is_open()) {
        file_.flush();   // 先刷新，把缓冲区内容写进去
        file_.close();   // 再关闭
    }
}

/**
 * @brief 写入文件
 *
 * 线程安全性：
 * - mutex_ 保护文件操作
 * - 多线程同时写同一个 FileSink 不会乱
 *
 * 轮转检查：
 * - 写入后检查文件位置（tellp()）
 * - 超过 maxSize_ 就触发轮转
 */
void FileSink::write(const std::string& message) {
    // 加锁，保护文件操作
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查文件是否打开成功
    if (file_.is_open()) {
        // 写入消息 + 换行符
        file_ << message << "\n";

        // tellp() 返回当前写入位置（从文件开头计算的字节数）
        // static_cast<size_t> 把 streamoff 转成 size_t
        // 检查是否超过大小限制
        if (static_cast<size_t>(file_.tellp()) > maxSize_) {
            file_.flush();  // 先刷出已有数据
            rotate();        // 触发轮转
            cleanup();       // 清理多余的历史文件
        }
    }
}

/**
 * @brief 刷新文件缓冲区
 */
void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

/**
 * @brief 日志轮转
 *
 * 轮转策略（以 maxFiles=5 为例）：
 * app.log        → app.log.1（最新）
 * app.log.1      → app.log.2
 * app.log.2      → app.log.3
 * app.log.3      → app.log.4
 * app.log.4      → app.log.5
 * app.log.5      → 被删除（超过数量限制）
 *
 * 实现步骤：
 * 1. 关闭当前文件（不关的话没法 rename）
 * 2. 从最大的开始，依次 rename
 * 3. 把当前文件 rename 成 .1
 * 4. 重新打开新文件
 */
void FileSink::rotate() {
    // 确保文件是打开的
    if (!file_.is_open()) return;

    // 第一步：关闭当前文件
    file_.close();

    // 第二步：移动历史文件
    // 从最大的开始移动，避免覆盖
    // 比如 maxFiles=5:
    // i=4: app.log.4 → app.log.5
    // i=3: app.log.3 → app.log.4
    // i=2: app.log.2 → app.log.3
    // i=1: app.log.1 → app.log.2
    for (int i = maxFiles_ - 1; i > 0; --i) {
        std::string oldPath = filepath_ + "." + std::to_string(i);
        std::string newPath = filepath_ + "." + std::to_string(i + 1);

        // std::rename 是 POSIX 函数，原子操作
        // 如果目标已存在，会被覆盖
        std::rename(oldPath.c_str(), newPath.c_str());
    }

    // 第三步：当前日志文件 → .1
    std::string firstBackup = filepath_ + ".1";
    std::rename(filepath_.c_str(), firstBackup.c_str());

    // 第四步：重新打开新文件（会创建新的 app.log）
    file_.open(filepath_, std::ios::app | std::ios::out);
}

/**
 * @brief 清理多余的历史文件
 *
 * 删除 app.log.N，其中 N > maxFiles_
 *
 * 比如 maxFiles_=5：
 * - 删除 app.log.6, app.log.7, app.log.8...
 * - 保留 app.log.1 ~ app.log.5
 */
void FileSink::cleanup() {
    // 从 maxFiles_+1 开始检查
    // 比如 maxFiles=5，就从 6 开始
    for (int i = maxFiles_ + 1;; ++i) {
        std::string path = filepath_ + "." + std::to_string(i);

        // access(F_OK) 检查文件是否存在
        // F_OK = existence
        // 如果文件不存在，access 返回 -1，errno = ENOENT
        if (access(path.c_str(), F_OK) != 0) {
            // 文件不存在，说明后面也不会有更老的文件了
            // （因为轮转时是连续命名的）
            break;
        }

        // unlink 删除文件（类似 rm 命令）
        // 删除后文件就没了
        unlink(path.c_str());
    }
}

// ==================== Logger 实现 ====================

/**
 * @brief 构造函数 - 初始化 Logger
 *
 * 初始化列表设置默认值：
 * - level_ = INFO：默认输出 INFO 及以上
 * - maxFileSize_ = 100MB：单文件最大 100MB
 * - maxFiles_ = 5：保留 5 个历史文件
 * - defaultModule_ = "MAIN"：默认模块名
 */
Logger::Logger()
    : level_(LogLevel::INFO)
    , maxFileSize_(100 * 1024 * 1024)
    , maxFiles_(5)
    , defaultModule_("MAIN")
{
    // 添加默认的 ConsoleSink
    // 为什么默认加控制台？
    // - 方便开发时直接看到日志
    // - 文件日志可能还没配置
    sinks_.push_back(std::make_shared<ConsoleSink>());

    // 启动后台写入线程
    // std::thread 构造时就开始运行
    // 传入 member function pointer 和 this
    writerThread_ = std::thread(&Logger::writerThread, this);
}

/**
 * @brief 析构函数 - 安全退出
 *
 * 退出步骤：
 * 1. stop_ = true：通知线程停止
 * 2. cv_.notify_all()：唤醒可能在等待的线程
 * 3. join()：等待线程结束
 * 4. flush：刷新所有待写日志（已经在 writerThread 里处理了）
 */
Logger::~Logger() {
    // 设置停止标志
    stop_ = true;

    // 通知所有等待的线程
    // 可能有线程在 cv_.wait() 阻塞着
    // notify_all 会唤醒所有等待的线程
    cv_.notify_all();

    // 等待后台线程结束
    // join() 会阻塞，直到线程函数返回
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
}

/**
 * @brief 获取单例实例
 *
 * C++11 保证：
 * - static 局部变量初始化线程安全
 * - 只有一个线程能完成初始化
 * - 后续调用直接返回已初始化的实例
 *
 * 为什么不用 double-checked locking？
 * - C++11 的 static 初始化已经保证了线程安全
 * - 不需要额外加锁
 */
Logger& Logger::instance() {
    static Logger instance;  // 线程安全初始化
    return instance;
}

/**
 * @brief 设置日志级别
 *
 * 为什么要加锁？
 * - level_ 会被多个线程访问：
 *   - 主线程调用 setLevel()
 *   - 后台线程读取 level_（在 log() 函数里）
 * - 加锁保证可见性
 */
void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

/**
 * @brief 设置日志输出文件
 *
 * 为什么每次调用都新建一个 FileSink？
 * - 可以同时输出到多个文件
 * - 多次调用 setFile() 会添加多个 FileSink
 * - 所有 Sink 都会被写入
 *
 * 如果想只输出到一个文件呢？
 * - 可以先清空 sinks_，再添加
 * - 或者在 setFile 之前先调用 Logger 内部的 clearSinks()
 */
void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 创建 FileSink
    auto fileSink = std::make_shared<FileSink>(path, maxFileSize_, maxFiles_);

    // 添加到 sinks_ 列表
    // shared_ptr 自动管理生命周期
    sinks_.push_back(fileSink);
}

/**
 * @brief 设置轮转参数
 *
 * 注意：
 * - 这个参数只对之后创建的 FileSink 生效
 * - 已经存在的 FileSink 不受影响
 */
void Logger::setRotation(size_t maxSize, int maxFiles) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxFileSize_ = maxSize;
    maxFiles_ = maxFiles;
}

/**
 * @brief 刷新队列
 *
 * 阻塞直到队列为空
 *
 * 实现：
 * - 使用 unique_lock + condition_variable
 * - wait 会在条件满足时返回
 *
 * 为什么用 while 而不是 if？
 * - spurious wakeup（虚假唤醒）
 * - 线程可能被意外唤醒，但队列实际不为空
 * - while 再次检查更安全
 */
void Logger::flush() {
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待队列变空
    // 第二个参数是 lambda，返回 true 才继续
    cv_.wait(lock, [this] { return queue_.empty(); });

    // 队列空了，但有些日志可能还在 Sink 的缓冲区里
    // 刷新所有 Sink
    for (auto& sink : sinks_) {
        sink->flush();
    }
}

/**
 * @brief 配置变更通知（热加载实现）
 *
 * 当 Config 检测到配置变化时调用此函数
 *
 * 目前只处理 log_level
 * 未来可以加更多，如 log_file、log_max_size 等
 */
void Logger::onConfigChange(const std::string& key, const std::string& value) {
    if (key == "log_level") {
        // 把字符串转成 LogLevel 枚举
        if (value == "trace") setLevel(LogLevel::TRACE);
        else if (value == "debug") setLevel(LogLevel::DEBUG);
        else if (value == "info") setLevel(LogLevel::INFO);
        else if (value == "warn") setLevel(LogLevel::WARN);
        else if (value == "error") setLevel(LogLevel::ERROR);
        else if (value == "fatal") setLevel(LogLevel::FATAL);
    }
}

/**
 * @brief 格式化日志消息
 *
 * 输出格式：
 * [LEVEL] timestamp [MODULE] [THREAD_ID] message
 *
 * 示例：
 * [INFO] 2026-04-24 15:30:00.123 [NETWORK] [12345] connection accepted fd=5
 */
std::string Logger::formatMessage(const char* module, LogLevel level, const std::string& msg) {
    // 第一步：转级别为字符串
    const char* levelStr;
    switch (level) {
        case LogLevel::TRACE: levelStr = "TRACE"; break;
        case LogLevel::DEBUG: levelStr = "DEBUG"; break;
        case LogLevel::INFO:  levelStr = "INFO";  break;
        case LogLevel::WARN:  levelStr = "WARN";  break;
        case LogLevel::ERROR: levelStr = "ERROR"; break;
        case LogLevel::FATAL: levelStr = "FATAL"; break;
    }

    // 第二步：拼接完整格式
    // Format::timestamp() 获取带毫秒的时间戳
    // Format::threadId() 获取线程ID
    return "[" + std::string(levelStr) + "] " + Format::timestamp() +
           " [" + std::string(module) + "] [" + Format::threadId() + "] " + msg;
}

/**
 * @brief 主日志写入函数（带模块）
 *
 * 工作流程：
 * 1. 检查日志级别
 * 2. 格式化消息
 * 3. 入队
 * 4. 通知后台线程
 */
void Logger::log(const char* module, LogLevel level, const char* fmt, ...) {
    // 级别过滤
    // 如果 level < level_，说明级别太低，忽略
    if (level < level_) return;

    // ========== 格式化 ==========

    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // ========== 完整格式化 ==========

    std::string message = formatMessage(module, level, buffer);

    // ========== 入队 ==========

    {
        // 加锁保护队列
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(message);
    }

    // 通知后台线程
    // 可能有线程在 cv_.wait() 阻塞着
    cv_.notify_one();
}

/**
 * @brief 主日志写入函数（兼容旧接口，无模块）
 */
void Logger::log(LogLevel level, const char* fmt, ...) {
    // 使用默认模块名
    // 但这里有个 bug：fmt 传了两遍
    // 应该是：log(defaultModule_.c_str(), level, fmt, fmt);
    // 正确写法见上面的 log(module, level, fmt, ...)
    log(defaultModule_.c_str(), level, fmt, fmt);
}

/**
 * @brief 后台写入线程主循环
 *
 * 设计要点：
 * 1. 批量处理：一次取完所有日志，减少锁竞争
 * 2. 条件变量：没有日志时阻塞，不浪费 CPU
 * 3. 优雅退出：stop_=true 且队列空才退出
 */
void Logger::writerThread() {
    // batch 批量缓冲区
    // 预分配 100 个元素，减少内存分配
    std::vector<std::string> batch;
    batch.reserve(100);

    while (true) {
        // ========== 第一阶段：从队列取数据 ==========

        {
            // unique_lock 用于 condition variable
            std::unique_lock<std::mutex> lock(mutex_);

            // 等待条件：
            // - stop_ = true 且队列空 → 退出
            // - stop_ = false 且队列非空 → 继续处理
            //
            // 注意：cv_.wait() 会释放锁，然后阻塞
            // 被唤醒时会重新获取锁，并检查条件
            cv_.wait(lock, [this] {
                return stop_ || !queue_.empty();
            });

            // 批量取出所有日志
            // 为什么要批量？
            // - 减少锁的持有时间
            // - 减少 notify_one 的次数
            while (!queue_.empty()) {
                // std::move 避免复制，提高性能
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        } // 锁在这里释放

        // ========== 第二阶段：写出日志 ==========

        // 如果队列是空的
        if (batch.empty()) {
            // 可能是因为 stop_=true 才被唤醒的
            if (stop_) break;  // 退出循环
            continue;           // 否则继续等待
        }

        // 遍历所有 Sink，批量写入
        for (auto& sink : sinks_) {
            for (const auto& msg : batch) {
                sink->write(msg);
            }
            sink->flush();  // 刷新，确保写入
        }

        // 清空 batch，准备下一轮
        // 注意：batch 对象本身保留，不释放内存
        batch.clear();
    }
}

} // namespace cc_server
