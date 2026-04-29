#ifndef CONCURRENTCACHE_TRACE_LOGGER_H
#define CONCURRENTCACHE_TRACE_LOGGER_H

#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <functional>
#include <thread>

namespace cc_server {
namespace testing {

// 操作类型枚举
enum class OpType {
    LOCK,
    UNLOCK,
    TRY_LOCK,
    READ,
    WRITE,
    ALLOC,
    FREE,
    THREAD_START,
    THREAD_END,
    TASK_SUBMIT,
    TASK_START,
    TASK_COMPLETE,
    WAIT,
    NOTIFY,
    CUSTOM
};

// 轨迹事件结构
struct TraceEvent {
    uint64_t timestamp_ns;     // 纳秒时间戳
    size_t thread_id;          // 线程ID (使用 std::hash<std::thread::id>)
    OpType op_type;           // 操作类型
    std::string location;     // 位置 "file:line" 或 "class::method"
    std::string target;       // 操作目标，如变量名、锁地址
    std::string value;        // 操作的值 "old->new" 或具体数值
    std::string thread_name;  // 线程名称

    std::string to_string() const {
        std::ostringstream oss;
        oss << "[" << std::setw(6) << timestamp_ns % 1000000000 << "] "
            << std::setw(8) << (thread_name.empty() ? "t" + std::to_string(thread_id) : thread_name) << " | "
            << std::setw(12) << op_type_to_string(op_type) << " | "
            << std::setw(24) << target << " | "
            << value;
        return oss.str();
    }

    static std::string op_type_to_string(OpType op) {
        switch (op) {
            case OpType::LOCK: return "LOCK";
            case OpType::UNLOCK: return "UNLOCK";
            case OpType::TRY_LOCK: return "TRY_LOCK";
            case OpType::READ: return "READ";
            case OpType::WRITE: return "WRITE";
            case OpType::ALLOC: return "ALLOC";
            case OpType::FREE: return "FREE";
            case OpType::THREAD_START: return "THREAD_START";
            case OpType::THREAD_END: return "THREAD_END";
            case OpType::TASK_SUBMIT: return "TASK_SUBMIT";
            case OpType::TASK_START: return "TASK_START";
            case OpType::TASK_COMPLETE: return "TASK_COMPLETE";
            case OpType::WAIT: return "WAIT";
            case OpType::NOTIFY: return "NOTIFY";
            case OpType::CUSTOM: return "CUSTOM";
            default: return "UNKNOWN";
        }
    }
};

// 锁获取信息（用于死锁检测）
struct LockInfo {
    size_t thread_id;
    std::string lock_addr;
    uint64_t timestamp_ns;
    bool is_read_lock;
    uint64_t lock_sequence_id;  // 用于追踪递归锁的序列号
};

// 内存操作信息
struct MemoryAccess {
    size_t thread_id;
    std::string addr;
    size_t size;
    bool is_write;
    uint64_t timestamp_ns;
};

class TraceLogger {
public:
    static TraceLogger& instance() {
        static TraceLogger inst;
        return inst;
    }

    void initialize(const std::string& test_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return;

        test_name_ = test_name;
        start_time_ = std::chrono::steady_clock::now();

        // 生成文件名
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << "logs/trace_" << test_name_ << "_" << time_t << ".log";
        log_file_path_ = oss.str();

        log_file_.open(log_file_path_, std::ios::out | std::ios::trunc);
        if (!log_file_.is_open()) {
            // 如果logs目录不存在，创建它
            std::ofstream dir_check("logs/.keep");
            log_file_.open(log_file_path_, std::ios::out | std::ios::trunc);
        }

        log_file_ << "========== TRACE LOG: " << test_name_ << " ==========\n";
        log_file_ << "Start time: " << time_t << "\n\n";
        log_file_.flush();

        initialized_ = true;
        event_count_ = 0;
    }

    // 重置日志状态
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (log_file_.is_open()) {
            log_file_.flush();
            log_file_.close();
        }

        events_.clear();
        lock_infos_.clear();
        memory_accesses_.clear();
        lock_hold_times_.clear();
        thread_lock_stack_.clear();

        initialized_ = false;
        event_count_ = 0;
        lock_sequence_counter_ = 0;
        test_name_.clear();
        log_file_path_.clear();
    }

    void log(OpType op, const std::string& location, const std::string& target,
             const std::string& value = "-", const std::string& thread_name = "") {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_) {
            initialize("unnamed_test");
        }

        TraceEvent event;
        event.timestamp_ns = get_timestamp_ns();
        event.thread_id = get_thread_id();
        event.op_type = op;
        event.location = location;
        event.target = target;
        event.value = value;
        event.thread_name = thread_name.empty() ? "t" + std::to_string(event.thread_id) : thread_name;

        events_.push_back(event);

        log_file_ << event.to_string() << "\n";
        event_count_++;

        if (event_count_ % 10000 == 0) {
            log_file_.flush();
        }

        // 更新锁状态跟踪
        update_lock_tracking(event);

        // 更新内存访问跟踪
        update_memory_tracking(event);
    }

    uint64_t get_timestamp_ns() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now - start_time_;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    }

    size_t get_thread_id() {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
    }

    // 获取所有事件（用于分析）
    const std::vector<TraceEvent>& get_events() const {
        return events_;
    }

    // 获取锁跟踪信息
    const std::vector<LockInfo>& get_lock_infos() const {
        return lock_infos_;
    }

    // 获取内存访问信息
    const std::vector<MemoryAccess>& get_memory_accesses() const {
        return memory_accesses_;
    }

    // 刷新并关闭
    void flush_and_close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (log_file_.is_open()) {
            log_file_.flush();
            log_file_.close();
        }
    }

    std::string get_log_file_path() const {
        return log_file_path_;
    }

    // 线程安全的自定义操作记录
    template<typename... Args>
    void custom(const std::string& location, const std::string& target,
                const std::string& format, Args... args) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), format.c_str(), args...);
        log(OpType::CUSTOM, location, target, buffer);
    }

private:
    TraceLogger() : initialized_(false), event_count_(0), start_time_() {}

    TraceLogger(const TraceLogger&) = delete;
    TraceLogger& operator=(const TraceLogger&) = delete;

    void update_lock_tracking(const TraceEvent& event) {
        if (event.op_type == OpType::LOCK || event.op_type == OpType::TRY_LOCK) {
            // 为递归锁生成唯一序列号
            uint64_t seq_id = ++lock_sequence_counter_;
            lock_infos_.push_back({event.thread_id, event.target, event.timestamp_ns, false, seq_id});

            // 追踪线程的锁获取顺序栈（用于死锁检测）
            thread_lock_stack_[event.thread_id].push_back({event.target, seq_id});
        } else if (event.op_type == OpType::UNLOCK) {
            // 找到对应的锁释放
            auto& stack = thread_lock_stack_[event.thread_id];
            for (auto it = lock_infos_.rbegin(); it != lock_infos_.rend(); ++it) {
                if (it->thread_id == event.thread_id && it->lock_addr == event.target) {
                    // 计算持有时间
                    uint64_t held_ns = event.timestamp_ns - it->timestamp_ns;
                    lock_hold_times_.push_back({it->lock_addr, held_ns});

                    // 从栈中移除对应的锁记录
                    for (auto stack_it = stack.rbegin(); stack_it != stack.rend(); ++stack_it) {
                        if (stack_it->first == event.target) {
                            stack.erase(std::next(stack_it).base());
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }

    void update_memory_tracking(const TraceEvent& event) {
        if (event.op_type == OpType::ALLOC || event.op_type == OpType::FREE ||
            event.op_type == OpType::READ || event.op_type == OpType::WRITE) {
            MemoryAccess access;
            access.thread_id = event.thread_id;
            access.addr = event.target;
            access.is_write = (event.op_type == OpType::WRITE || event.op_type == OpType::ALLOC);
            access.timestamp_ns = event.timestamp_ns;
            memory_accesses_.push_back(access);
        }
    }

    bool initialized_;
    uint64_t event_count_;
    std::chrono::steady_clock::time_point start_time_;
    std::string test_name_;
    std::string log_file_path_;
    std::ofstream log_file_;
    std::mutex mutex_;

    std::vector<TraceEvent> events_;
    std::vector<LockInfo> lock_infos_;
    std::vector<MemoryAccess> memory_accesses_;
    std::vector<std::pair<std::string, uint64_t>> lock_hold_times_; // lock_addr, hold_time_ns
    std::unordered_map<size_t, std::vector<std::pair<std::string, uint64_t>>> thread_lock_stack_; // thread_id -> (lock_addr, seq_id)
    uint64_t lock_sequence_counter_ = 0;
};

// 辅助宏 - 自动捕获文件名和行号
#define TRACE_LOG(op, target, value) \
    cc_server::testing::TraceLogger::instance().log(op, __FILE__ ":" + std::to_string(__LINE__), target, value)

#define TRACE_LOG_NAMED(op, target, value, name) \
    cc_server::testing::TraceLogger::instance().log(op, __FILE__ ":" + std::to_string(__LINE__), target, value, name)

// 带格式的日志
#define TRACE_LOGF(op, target, fmt, ...) \
    cc_server::testing::TraceLogger::instance().custom(__FILE__ ":" + std::to_string(__LINE__), target, fmt, __VA_ARGS__)

} // namespace testing
} // namespace cc_server

#endif // CONCURRENTCACHE_TRACE_LOGGER_H
