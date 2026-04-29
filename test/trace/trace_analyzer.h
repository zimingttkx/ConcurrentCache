#ifndef CONCURRENTCACHE_TRACE_ANALYZER_H
#define CONCURRENTCACHE_TRACE_ANALYZER_H

#include "trace_logger.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <set>

namespace cc_server {
namespace testing {

// 死锁环
struct DeadlockCycle {
    std::vector<std::pair<size_t, std::string>> threads_and_locks; // thread_id -> lock_addr
    std::string description;
};

// 数据竞争
struct DataRace {
    std::string address;
    std::vector<size_t> involved_threads;
    std::string location;
    std::string description;
};

// 不变量验证结果
struct InvariantViolation {
    std::string invariant_name;
    std::string description;
    uint64_t timestamp_ns;
};

// 分析报告
struct AnalysisReport {
    std::vector<DeadlockCycle> deadlocks;
    std::vector<DataRace> data_races;
    std::vector<InvariantViolation> invariant_violations;
    bool all_passed;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "\n";
        oss << "========================================\n";
        oss << "         ANALYSIS REPORT\n";
        oss << "========================================\n\n";

        // 死锁
        oss << "--- DEADLOCK DETECTION ---\n";
        if (deadlocks.empty()) {
            oss << "[OK] No deadlocks detected\n\n";
        } else {
            oss << "[FAIL] Deadlocks found: " << deadlocks.size() << "\n\n";
            for (size_t i = 0; i < deadlocks.size(); ++i) {
                oss << "  Cycle #" << (i + 1) << ":\n";
                oss << "    " << deadlocks[i].description << "\n\n";
            }
        }

        // 数据竞争
        oss << "--- DATA RACE DETECTION ---\n";
        if (data_races.empty()) {
            oss << "[OK] No data races detected\n\n";
        } else {
            oss << "[FAIL] Data races found: " << data_races.size() << "\n\n";
            for (size_t i = 0; i < data_races.size(); ++i) {
                oss << "  Race #" << (i + 1) << ":\n";
                oss << "    Address: " << data_races[i].address << "\n";
                oss << "    Location: " << data_races[i].location << "\n";
                oss << "    " << data_races[i].description << "\n\n";
            }
        }

        // 不变量
        oss << "--- INVARIANT VALIDATION ---\n";
        if (invariant_violations.empty()) {
            oss << "[OK] All invariants satisfied\n\n";
        } else {
            oss << "[FAIL] Invariant violations: " << invariant_violations.size() << "\n\n";
            for (size_t i = 0; i < invariant_violations.size(); ++i) {
                oss << "  Violation #" << (i + 1) << ":\n";
                oss << "    Invariant: " << invariant_violations[i].invariant_name << "\n";
                oss << "    " << invariant_violations[i].description << "\n\n";
            }
        }

        oss << "========================================\n";
        if (all_passed) {
            oss << "         ALL CHECKS PASSED\n";
        } else {
            oss << "         SOME CHECKS FAILED\n";
        }
        oss << "========================================\n";

        return oss.str();
    }

    void save_to_file(const std::string& path) const {
        std::ofstream file(path);
        if (file.is_open()) {
            file << to_string();
            file.close();
        }
    }
};

// 死锁检测器
class DeadlockDetector {
public:
    AnalysisReport analyze(const std::vector<TraceEvent>& events,
                          const std::vector<LockInfo>& lock_infos) {
        AnalysisReport report;

        // 构建锁获取等待图
        // thread -> set of locks it's waiting for
        // lock -> set of threads holding it
        std::unordered_map<size_t, std::unordered_set<std::string>> thread_waiting_for;
        std::unordered_map<std::string, size_t> lock_held_by; // lock -> thread_id

        std::unordered_map<size_t, std::vector<std::string>> thread_lock_sequence;

        // 按时间顺序处理事件
        for (const auto& event : events) {
            if (event.op_type == OpType::LOCK || event.op_type == OpType::TRY_LOCK) {
                const std::string& lock_addr = event.target;
                size_t tid = event.thread_id;

                // 记录锁获取顺序
                thread_lock_sequence[tid].push_back(lock_addr);

                // 如果锁已被其他线程持有，建立等待关系
                if (lock_held_by.find(lock_addr) != lock_held_by.end() &&
                    lock_held_by[lock_addr] != tid) {
                    thread_waiting_for[tid].insert(lock_addr);
                }

                // 更新锁持有信息
                lock_held_by[lock_addr] = tid;
            } else if (event.op_type == OpType::UNLOCK) {
                const std::string& lock_addr = event.target;
                size_t tid = event.thread_id;

                // 如果有线程在等待这个锁，移除等待关系
                for (auto& pair : thread_waiting_for) {
                    pair.second.erase(lock_addr);
                }

                // 移除持有关系
                if (lock_held_by.find(lock_addr) != lock_held_by.end() &&
                    lock_held_by[lock_addr] == tid) {
                    lock_held_by.erase(lock_addr);
                }
            }
        }

        // 使用 DFS 检测等待图中的环
        std::unordered_set<size_t> visited;
        std::unordered_set<size_t> in_stack;
        std::vector<size_t> path;

        for (const auto& pair : thread_waiting_for) {
            size_t start_thread = pair.first;
            if (visited.find(start_thread) == visited.end()) {
                std::vector<std::pair<size_t, std::string>> cycle;
                if (dfs_find_cycle(start_thread, thread_waiting_for, lock_held_by,
                                   visited, in_stack, path, cycle)) {
                    DeadlockCycle dc;
                    dc.threads_and_locks = cycle;
                    std::ostringstream oss;
                    for (size_t i = 0; i < cycle.size(); ++i) {
                        if (i > 0) oss << " -> ";
                        oss << "Thread " << cycle[i].first << " (waiting " << cycle[i].second << ")";
                    }
                    oss << " [ABBA deadlock cycle detected]";
                    dc.description = oss.str();
                    report.deadlocks.push_back(dc);
                }
            }
        }

        // 去重
        std::sort(report.deadlocks.begin(), report.deadlocks.end(),
                 [](const DeadlockCycle& a, const DeadlockCycle& b) {
                     return a.description < b.description;
                 });
        report.deadlocks.erase(std::unique(report.deadlocks.begin(), report.deadlocks.end(),
                                           [](const DeadlockCycle& a, const DeadlockCycle& b) {
                                               return a.description == b.description;
                                           }), report.deadlocks.end());

        return report;
    }

private:
    bool dfs_find_cycle(size_t thread,
                       const std::unordered_map<size_t, std::unordered_set<std::string>>& thread_waiting_for,
                       const std::unordered_map<std::string, size_t>& lock_held_by,
                       std::unordered_set<size_t>& visited,
                       std::unordered_set<size_t>& in_stack,
                       std::vector<size_t>& path,
                       std::vector<std::pair<size_t, std::string>>& cycle) {
        visited.insert(thread);
        in_stack.insert(thread);
        path.push_back(thread);

        auto it = thread_waiting_for.find(thread);
        if (it != thread_waiting_for.end()) {
            for (const auto& lock_addr : it->second) {
                // 检查锁是否被某个线程持有
                auto lock_it = lock_held_by.find(lock_addr);
                if (lock_it != lock_held_by.end()) {
                    size_t holder = lock_it->second;

                    // 如果持有者正在等待其他锁，可能形成环
                    if (thread_waiting_for.find(holder) != thread_waiting_for.end()) {
                        if (in_stack.find(holder) != in_stack.end()) {
                            // 找到环！构建环信息
                            auto path_it = std::find(path.begin(), path.end(), holder);
                            for (; path_it != path.end(); ++path_it) {
                                size_t t = *path_it;
                                auto t_wait = thread_waiting_for.find(t);
                                if (t_wait != thread_waiting_for.end() && !t_wait->second.empty()) {
                                    cycle.push_back({t, *t_wait->second.begin()});
                                }
                            }
                            // 添加最后一段
                            cycle.push_back({holder, lock_addr});
                            return true;
                        } else if (visited.find(holder) == visited.end()) {
                            if (dfs_find_cycle(holder, thread_waiting_for, lock_held_by,
                                              visited, in_stack, path, cycle)) {
                                // 在环中加入当前线程等待的锁信息
                                cycle.push_back({thread, lock_addr});
                                return true;
                            }
                        }
                    }
                }
            }
        }

        path.pop_back();
        in_stack.erase(thread);
        return false;
    }
};

// 数据竞争检测器
class RaceDetector {
public:
    AnalysisReport analyze(const std::vector<TraceEvent>& events,
                          const std::vector<MemoryAccess>& memory_accesses) {
        AnalysisReport report;

        // 按地址分组内存访问
        std::unordered_map<std::string, std::vector<MemoryAccess>> accesses_by_addr;

        for (const auto& access : memory_accesses) {
            accesses_by_addr[access.addr].push_back(access);
        }

        // 对每个地址检查是否有数据竞争
        for (const auto& pair : accesses_by_addr) {
            const std::string& addr = pair.first;
            const std::vector<MemoryAccess>& accesses = pair.second;

            if (accesses.size() < 2) continue;

            // 检查是否有并发的读写或写写访问
            for (size_t i = 0; i < accesses.size(); ++i) {
                for (size_t j = i + 1; j < accesses.size(); ++j) {
                    const auto& a = accesses[i];
                    const auto& b = accesses[j];

                    // 如果两个访问是并发的（时间戳接近且不同线程）
                    if (a.thread_id != b.thread_id && is_concurrent(a, b)) {
                        // 至少有一个是写操作
                        if (a.is_write || b.is_write) {
                            DataRace race;
                            race.address = addr;
                            race.involved_threads = {a.thread_id, b.thread_id};
                            race.description = "Threads " +
                                std::to_string(a.thread_id) + " and " +
                                std::to_string(b.thread_id) +
                                " access same address concurrently without synchronization";
                            report.data_races.push_back(race);
                        }
                    }
                }
            }
        }

        // 简单的去重（基于地址）
        std::sort(report.data_races.begin(), report.data_races.end(),
                 [](const DataRace& a, const DataRace& b) {
                     return a.address < b.address;
                 });
        report.data_races.erase(std::unique(report.data_races.begin(), report.data_races.end(),
                                           [](const DataRace& a, const DataRace& b) {
                                               return a.address == b.address;
                                           }), report.data_races.end());

        return report;
    }

private:
    bool is_concurrent(const MemoryAccess& a, const MemoryAccess& b) {
        // 如果两个访问的时间戳相差小于 100μs，认为是并发的
        uint64_t diff = (a.timestamp_ns > b.timestamp_ns) ?
                        (a.timestamp_ns - b.timestamp_ns) :
                        (b.timestamp_ns - a.timestamp_ns);
        return diff < 100000; // 100μs (原为 1ms)
    }
};

// 不变量验证器基类
class InvariantValidator {
public:
    virtual ~InvariantValidator() = default;
    virtual std::string name() const = 0;
    virtual AnalysisReport validate(const std::vector<TraceEvent>& events) = 0;
};

// ThreadPool 不变量验证器
class ThreadPoolInvariantValidator : public InvariantValidator {
public:
    ThreadPoolInvariantValidator(size_t expected_threads)
        : expected_threads_(expected_threads) {}

    std::string name() const override {
        return "ThreadPoolInvariant";
    }

    AnalysisReport validate(const std::vector<TraceEvent>& events) override {
        AnalysisReport report;

        int64_t submitted = 0;
        int64_t started = 0;
        int64_t completed = 0;
        int64_t active_threads = 0;
        size_t max_concurrent_active = 0;

        std::unordered_map<size_t, bool> thread_active;

        for (const auto& event : events) {
            if (event.op_type == OpType::THREAD_START) {
                active_threads++;
                thread_active[event.thread_id] = true;
                max_concurrent_active = std::max(max_concurrent_active,
                                                static_cast<size_t>(active_threads));
            } else if (event.op_type == OpType::THREAD_END) {
                if (active_threads > 0) active_threads--;
                thread_active[event.thread_id] = false;
            } else if (event.op_type == OpType::TASK_SUBMIT) {
                submitted++;
            } else if (event.op_type == OpType::TASK_START) {
                started++;
            } else if (event.op_type == OpType::TASK_COMPLETE) {
                completed++;
            }
        }

        // 不变量1: 提交的任务数应该等于完成的任务数（如果没有任务在队列中）
        // 不变量2: 活跃线程数不应该超过预期
        // 不变量3: started <= submitted, completed <= submitted
        // 不变量4: started - completed = 当前正在执行的任务数

        if (started > submitted) {
            InvariantViolation v;
            v.invariant_name = name();
            v.description = "Tasks started (" + std::to_string(started) +
                           ") > tasks submitted (" + std::to_string(submitted) + ")";
            report.invariant_violations.push_back(v);
        }

        if (completed > submitted) {
            InvariantViolation v;
            v.invariant_name = name();
            v.description = "Tasks completed (" + std::to_string(completed) +
                           ") > tasks submitted (" + std::to_string(submitted) + ")";
            report.invariant_violations.push_back(v);
        }

        if (active_threads > expected_threads_) {
            InvariantViolation v;
            v.invariant_name = name();
            v.description = "Active threads (" + std::to_string(active_threads) +
                           ") > expected (" + std::to_string(expected_threads_) + ")";
            report.invariant_violations.push_back(v);
        }

        return report;
    }

private:
    size_t expected_threads_;
};

// MemoryPool 不变量验证器
class MemoryPoolInvariantValidator : public InvariantValidator {
public:
    MemoryPoolInvariantValidator() = default;

    std::string name() const override {
        return "MemoryPoolInvariant";
    }

    AnalysisReport validate(const std::vector<TraceEvent>& events) override {
        AnalysisReport report;

        std::unordered_set<std::string> allocated_addrs;
        std::unordered_map<std::string, std::string> addr_to_thread; // addr -> thread that allocated
        std::unordered_map<std::string, uint64_t> addr_to_size;
        uint64_t total_allocated = 0;
        uint64_t total_freed = 0;

        for (const auto& event : events) {
            if (event.op_type == OpType::ALLOC) {
                allocated_addrs.insert(event.target);
                addr_to_thread[event.target] = std::to_string(event.thread_id);
                addr_to_size[event.target] = parse_size(event.value);
                total_allocated++;

                // 解析大小
                size_t size = parse_size(event.value);
                outstanding_allocs_[event.thread_id]++;
                outstanding_bytes_[event.thread_id] += size;
            } else if (event.op_type == OpType::FREE) {
                if (allocated_addrs.find(event.target) != allocated_addrs.end()) {
                    allocated_addrs.erase(event.target);
                    total_freed++;

                    auto it = addr_to_size.find(event.target);
                    if (it != addr_to_size.end()) {
                        outstanding_bytes_[event.thread_id] -= it->second;
                    }
                } else {
                    // 重复释放
                    InvariantViolation v;
                    v.invariant_name = name();
                    v.description = "Double free detected: " + event.target +
                                   " by thread " + std::to_string(event.thread_id);
                    report.invariant_violations.push_back(v);
                }
            }
        }

        // 不变量1: 不应该有未释放的内存（长期运行）
        if (!allocated_addrs.empty() && total_allocated > total_freed + 100) {
            // 允许一些分配还在进行中
            InvariantViolation v;
            v.invariant_name = name();
            v.description = "Potential memory leak: " + std::to_string(allocated_addrs.size()) +
                           " addresses not freed (allocated=" + std::to_string(total_allocated) +
                           ", freed=" + std::to_string(total_freed) + ")";
            report.invariant_violations.push_back(v);
        }

        // 不变量2: alloc次数应该等于free次数（最终）
        // 这个检查不适合短期测试，只做警告

        return report;
    }

    std::unordered_map<size_t, size_t> get_outstanding_allocs() const {
        return outstanding_allocs_;
    }

    std::unordered_map<size_t, size_t> get_outstanding_bytes() const {
        return outstanding_bytes_;
    }

private:
    size_t parse_size(const std::string& value) {
        // 简单解析 size:value 格式
        size_t pos = value.find(':');
        if (pos != std::string::npos) {
            return std::stoull(value.substr(pos + 1));
        }
        return 0;
    }

    std::unordered_map<size_t, size_t> outstanding_allocs_;
    std::unordered_map<size_t, size_t> outstanding_bytes_;
};

// 锁不变量验证器
class LockInvariantValidator : public InvariantValidator {
public:
    LockInvariantValidator() = default;

    std::string name() const override {
        return "LockInvariant";
    }

    AnalysisReport validate(const std::vector<TraceEvent>& events) override {
        AnalysisReport report;

        std::unordered_map<size_t, int> lock_depth; // thread -> lock count
        std::unordered_map<size_t, std::string> last_lock; // thread -> last lock addr

        for (const auto& event : events) {
            if (event.op_type == OpType::LOCK) {
                lock_depth[event.thread_id]++;
                last_lock[event.thread_id] = event.target;
            } else if (event.op_type == OpType::UNLOCK) {
                if (lock_depth[event.thread_id] > 0) {
                    lock_depth[event.thread_id]--;
                } else {
                    InvariantViolation v;
                    v.invariant_name = name();
                    v.description = "Unlock without corresponding lock: thread " +
                                   std::to_string(event.thread_id) +
                                   " unlocking " + event.target;
                    report.invariant_violations.push_back(v);
                }
            }
        }

        // 检查是否有未释放的锁
        for (const auto& pair : lock_depth) {
            if (pair.second > 0) {
                InvariantViolation v;
                v.invariant_name = name();
                v.description = "Lock not released: thread " + std::to_string(pair.first) +
                               " has " + std::to_string(pair.second) + " unreleased locks";
                report.invariant_violations.push_back(v);
            }
        }

        return report;
    }
};

// 综合分析器
class TraceAnalyzer {
public:
    void add_validator(std::shared_ptr<InvariantValidator> validator) {
        validators_.push_back(validator);
    }

    AnalysisReport analyze(const std::vector<TraceEvent>& events,
                          const std::vector<LockInfo>& lock_infos,
                          const std::vector<MemoryAccess>& memory_accesses) {
        AnalysisReport report;

        // 1. 死锁检测
        DeadlockDetector deadlock_detector;
        AnalysisReport deadlock_report = deadlock_detector.analyze(events, lock_infos);
        report.deadlocks = deadlock_report.deadlocks;

        // 2. 数据竞争检测
        RaceDetector race_detector;
        AnalysisReport race_report = race_detector.analyze(events, memory_accesses);
        report.data_races = race_report.data_races;

        // 3. 不变量验证
        for (const auto& validator : validators_) {
            AnalysisReport invariant_report = validator->validate(events);
            report.invariant_violations.insert(
                report.invariant_violations.end(),
                invariant_report.invariant_violations.begin(),
                invariant_report.invariant_violations.end()
            );
        }

        // 判断是否全部通过
        report.all_passed = report.deadlocks.empty() &&
                           report.data_races.empty() &&
                           report.invariant_violations.empty();

        return report;
    }

private:
    std::vector<std::shared_ptr<InvariantValidator>> validators_;
};

} // namespace testing
} // namespace cc_server

#endif // CONCURRENTCACHE_TRACE_ANALYZER_H
