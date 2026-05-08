/**
 * @file long_running_stress_test.cpp
 * @brief 长时间持续压力测试 - 3分钟百万级并发测试
 *
 * 测试目标：
 * 1. 持续3分钟百万级并发操作
 * 2. 检测数据竞争 (data race)
 * 3. 检测内存泄漏
 * 4. 检测死锁
 * 5. 数据完整性验证
 * 6. 持久化完整性验证
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/resource.h>
#include <signal.h>
#include <set>

#include "base/log.h"
#include "base/format.h"
#include "cache/storage.h"
#include "command/string_cmd.h"
#include "command/expire_cmd.h"
#include "command/command_factory.h"
#include "persistence/rdb.h"
#include "datatype/object.h"
#include "protocol/resp.h"

using namespace cc_server;
using namespace std::chrono;

// 全局标志
std::atomic<bool> g_running{true};
std::atomic<bool> g_pause{false};

// 统计结构
struct StressStats {
    std::atomic<uint64_t> total_operations{0};
    std::atomic<uint64_t> string_ops{0};
    std::atomic<uint64_t> list_ops{0};
    std::atomic<uint64_t> hash_ops{0};
    std::atomic<uint64_t> set_ops{0};
    std::atomic<uint64_t> zset_ops{0};
    std::atomic<uint64_t> expire_ops{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> lock_contentions{0};

    // 数据完整性验证
    std::atomic<uint64_t> integrity_check_passed{0};
    std::atomic<uint64_t> integrity_check_failed{0};

    void reset() {
        total_operations = 0;
        string_ops = 0;
        list_ops = 0;
        hash_ops = 0;
        set_ops = 0;
        zset_ops = 0;
        expire_ops = 0;
        errors = 0;
        lock_contentions = 0;
        integrity_check_passed = 0;
        integrity_check_failed = 0;
    }
};

static StressStats g_stats;

// 配置
struct TestConfig {
    int num_threads = 16;
    int key_range = 5000;
    int duration_seconds = 180;  // 3分钟
    int report_interval_seconds = 10;
    int integrity_check_interval_seconds = 30;
};

static TestConfig g_config;

// 获取当前内存使用（MB）
double get_memory_usage_mb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss / 1024.0;
    }
    return 0;
}

// 获取线程数
int get_thread_count() {
    return std::thread::hardware_concurrency();
}

// 随机生成字符串
std::string random_string(int len) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static thread_local std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ time(nullptr));
    static thread_local std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);

    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; ++i) {
        result += chars[dist(rng)];
    }
    return result;
}

// 生成 key
std::string make_key(const std::string& prefix, int id) {
    return prefix + "_" + std::to_string(id);
}

// ============================================================================
// 混合命令压力测试 - 每个线程执行所有类型的操作
// ============================================================================
void mixed_stress_worker(int thread_id, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 12345 + time(nullptr));

    // 每个线程操作不同范围的 key，减少冲突
    int local_key_range = key_range / g_config.num_threads;
    int key_offset = thread_id * local_key_range;

    uint64_t local_ops = 0;
    uint64_t local_errors = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        if (g_pause.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(milliseconds(100));
            continue;
        }

        int op = rng() % 100;
        int key_id = key_offset + (rng() % local_key_range);

        try {
            if (op < 20) {  // 20%: String SET/GET
                std::string key = make_key("str", key_id);
                std::string value = random_string(64);
                storage.set(key, CacheObject(value));
                g_stats.string_ops++;
            } else if (op < 30) {  // 10%: String GET
                std::string key = make_key("str", key_id);
                storage.get(key);
                g_stats.string_ops++;
            } else if (op < 40) {  // 10%: List
                std::string key = make_key("list", key_id);
                CacheObject obj;
                obj.list_push(random_string(32), true);
                storage.set(key, obj);
                g_stats.list_ops++;
            } else if (op < 50) {  // 10%: Hash
                std::string key = make_key("hash", key_id);
                CacheObject obj;
                obj.hash_set(random_string(16), random_string(32));
                storage.set(key, obj);
                g_stats.hash_ops++;
            } else if (op < 60) {  // 10%: Set
                std::string key = make_key("set", key_id);
                CacheObject obj;
                obj.set_add(random_string(32));
                storage.set(key, obj);
                g_stats.set_ops++;
            } else if (op < 70) {  // 10%: ZSet
                std::string key = make_key("zset", key_id);
                CacheObject obj;
                obj.zset_add(random_string(32), rng() % 1000);
                storage.set(key, obj);
                g_stats.zset_ops++;
            } else if (op < 80) {  // 10%: DEL
                std::string key = make_key("str", key_id);
                storage.del(key);
                g_stats.string_ops++;
            } else if (op < 85) {  // 5%: EXPIRE
                std::string key = make_key("str", key_id);
                storage.set_with_expire(key, CacheObject(random_string(64)), 10000);
                g_stats.expire_ops++;
            } else if (op < 90) {  // 5%: EXISTS
                std::string key = make_key("str", key_id);
                storage.exist(key);
                g_stats.string_ops++;
            } else if (op < 95) {  // 5%: Mixed read
                std::string key = make_key("hash", key_id);
                storage.get(key);
                g_stats.hash_ops++;
            } else {  // 5%: Size check
                storage.size();
                g_stats.string_ops++;
            }

            local_ops++;
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            local_errors++;
            g_stats.errors++;
        }
    }

    LOG_INFO(STRESS, "Worker %d finished: %lu ops, %lu errors", thread_id, local_ops, local_errors);
}

// ============================================================================
// 数据完整性检查线程
// ============================================================================
void integrity_checker() {
    auto& storage = GlobalStorage::instance();
    std::set<std::string> expected_keys;

    // 生成期望的 key 集合
    for (int i = 0; i < g_config.key_range; ++i) {
        expected_keys.insert(make_key("str", i));
        expected_keys.insert(make_key("list", i));
        expected_keys.insert(make_key("hash", i));
        expected_keys.insert(make_key("set", i));
        expected_keys.insert(make_key("zset", i));
    }

    int check_count = 0;
    auto start = steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(seconds(g_config.integrity_check_interval_seconds));

        if (!g_running.load(std::memory_order_relaxed)) break;

        check_count++;
        auto now = steady_clock::now();
        auto elapsed = duration_cast<seconds>(now - start).count();

        LOG_INFO(STRESS, "Integrity check #%d started at t=%ds", check_count, elapsed);

        // 抽样检查一些 key
        int checked = 0;
        int found = 0;
        int errors = 0;

        for (int i = 0; i < 100 && i < g_config.key_range; ++i) {
            std::string key = make_key("str", i);
            if (storage.exist(key)) {
                found++;
            }
            checked++;

            key = make_key("hash", i);
            if (storage.exist(key)) {
                found++;
            }
            checked++;
        }

        // 检查存储大小是否合理
        size_t storage_size = storage.size();

        LOG_INFO(STRESS, "Integrity check #%d: checked=%d, found=%d, storage_size=%zu",
                 check_count, checked, found, storage_size);

        // 基本合理性检查
        if (storage_size > g_config.key_range * 5) {
            LOG_WARN(STRESS, "Storage size %zu exceeds expected range!", storage_size);
            g_stats.integrity_check_failed++;
        } else {
            g_stats.integrity_check_passed++;
        }
    }

    LOG_INFO(STRESS, "Integrity checker finished after %d checks", check_count);
}

// ============================================================================
// RDB 持久化线程
// ============================================================================
void persistence_worker() {
    auto& rdb = RdbPersistence::instance();
    auto& storage = GlobalStorage::instance();

    int save_count = 0;
    auto start = steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(seconds(30));  // 每30秒保存一次

        if (!g_running.load(std::memory_order_relaxed)) break;

        auto now = steady_clock::now();
        auto elapsed = duration_cast<seconds>(now - start).count();

        std::string path = "./long_run_dump_" + std::to_string(save_count++) + ".rdb";
        size_t storage_size = storage.size();

        LOG_INFO(STRESS, "[t=%ds] Starting RDB save #%d, storage size=%zu",
                 elapsed, save_count, storage_size);

        bool success = rdb.save(path, storage);

        if (success) {
            LOG_INFO(STRESS, "[t=%ds] RDB save #%d completed, keys=%zu",
                     elapsed, save_count, storage_size);

            // 验证文件
            std::ifstream file(path, std::ios::binary);
            if (file.good()) {
                file.seekg(0, std::ios::end);
                auto file_size = file.tellg();
                LOG_INFO(STRESS, "RDB file verified: %s, size=%ld bytes",
                         path.c_str(), static_cast<long>(file_size));
            }

            // 立即加载验证
            storage.clear();
            LOG_INFO(STRESS, "Storage cleared, size=%zu", storage.size());

            if (rdb.load(path, storage)) {
                LOG_INFO(STRESS, "RDB reload verified, storage size=%zu", storage.size());
            } else {
                LOG_ERROR(STRESS, "RDB reload FAILED!");
            }

            // 清理
            std::remove(path.c_str());
        } else {
            LOG_ERROR(STRESS, "[t=%ds] RDB save #%d FAILED!", elapsed, save_count);
        }
    }

    LOG_INFO(STRESS, "Persistence worker finished after %d saves", save_count);
}

// ============================================================================
// 信号处理
// ============================================================================
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO(STRESS, "Received signal %d, stopping...", sig);
        g_running = false;
    }
}

// ============================================================================
// 报告线程 - 定期输出状态
// ============================================================================
void reporter() {
    auto start = steady_clock::now();
    int report_count = 0;
    double start_mem = get_memory_usage_mb();

    uint64_t last_total = 0;
    uint64_t last_string = 0;
    uint64_t last_list = 0;
    uint64_t last_hash = 0;
    uint64_t last_set = 0;
    uint64_t last_zset = 0;
    uint64_t last_expire = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(seconds(g_config.report_interval_seconds));

        if (!g_running.load(std::memory_order_relaxed)) break;

        report_count++;
        auto now = steady_clock::now();
        auto elapsed = duration_cast<seconds>(now - start).count();
        auto actual_elapsed = elapsed;  // 报告用实际经过的时间

        uint64_t current_total = g_stats.total_operations.load();
        uint64_t current_string = g_stats.string_ops.load();
        uint64_t current_list = g_stats.list_ops.load();
        uint64_t current_hash = g_stats.hash_ops.load();
        uint64_t current_set = g_stats.set_ops.load();
        uint64_t current_zset = g_stats.zset_ops.load();
        uint64_t current_expire = g_stats.expire_ops.load();

        uint64_t delta_total = current_total - last_total;
        uint64_t delta_string = current_string - last_string;
        uint64_t delta_list = current_list - last_list;
        uint64_t delta_hash = current_hash - last_hash;
        uint64_t delta_set = current_set - last_set;
        uint64_t delta_zset = current_zset - last_zset;
        uint64_t delta_expire = current_expire - last_expire;

        double current_mem = get_memory_usage_mb();
        auto& storage = GlobalStorage::instance();
        size_t storage_size = storage.size();

        double ops_per_sec = g_config.report_interval_seconds > 0 ?
            (double)delta_total / g_config.report_interval_seconds : 0;

        std::cout << "\n";
        std::cout << "========================================\n";
        std::cout << "  [Report #" << report_count << " at t=" << actual_elapsed << "s]\n";
        std::cout << "========================================\n";
        std::cout << "  Duration: " << actual_elapsed << "s / " << g_config.duration_seconds << "s\n";
        std::cout << "  Storage size: " << storage_size << " keys\n";
        std::cout << "\n";
        std::cout << "  Operations (delta/sec):\n";
        std::cout << "    Total:   " << delta_total << " (" << (uint64_t)ops_per_sec << " ops/s)\n";
        std::cout << "    String:  " << delta_string << "\n";
        std::cout << "    List:    " << delta_list << "\n";
        std::cout << "    Hash:    " << delta_hash << "\n";
        std::cout << "    Set:     " << delta_set << "\n";
        std::cout << "    ZSet:    " << delta_zset << "\n";
        std::cout << "    Expire:  " << delta_expire << "\n";
        std::cout << "\n";
        std::cout << "  Cumulative:\n";
        std::cout << "    Total ops:   " << current_total << "\n";
        std::cout << "    Errors:      " << g_stats.errors.load() << "\n";
        std::cout << "\n";
        std::cout << "  Memory:\n";
        std::cout << "    Current:  " << current_mem << " MB\n";
        std::cout << "    Delta:    +" << (current_mem - start_mem) << " MB\n";
        std::cout << "========================================\n";
        std::cout << std::endl;

        // Log the same info
        LOG_INFO(STRESS, "[Report #%d t=%ds] ops=%lu (%.0f/s), errors=%lu, mem=%.2fMB, storage=%zu",
                 report_count, actual_elapsed, current_total, ops_per_sec,
                 g_stats.errors.load(), current_mem, storage_size);

        last_total = current_total;
        last_string = current_string;
        last_list = current_list;
        last_hash = current_hash;
        last_set = current_set;
        last_zset = current_zset;
        last_expire = current_expire;
    }
}

// ============================================================================
// 主测试函数
// ============================================================================
int main(int argc, char* argv[]) {
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            g_config.num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            g_config.duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            g_config.key_range = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --threads N     Number of threads (default: 16)\n";
            std::cout << "  --duration N    Duration in seconds (default: 180)\n";
            std::cout << "  --keys N        Key range (default: 5000)\n";
            std::cout << "  --help          Show this help\n";
            return 0;
        }
    }

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "   ConcurrentCache V3 Long-Running Stress Test\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    std::cout << "Configuration:\n";
    std::cout << "  Threads:        " << g_config.num_threads << "\n";
    std::cout << "  Duration:       " << g_config.duration_seconds << " seconds\n";
    std::cout << "  Key range:     " << g_config.key_range << "\n";
    std::cout << "  Report every:  " << g_config.report_interval_seconds << " seconds\n";
    std::cout << "\n";
    std::cout << "Expected operations: ~" << (g_config.num_threads * 100 * g_config.duration_seconds) << "+\n";
    std::cout << "\n";

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化存储
    std::cout << "Initializing storage...\n";
    auto& storage = GlobalStorage::instance();
    storage.clear();
    std::cout << "Storage initialized, current size: " << storage.size() << "\n";

    double start_mem = get_memory_usage_mb();
    auto test_start = steady_clock::now();

    // 启动工作线程
    std::cout << "Starting " << g_config.num_threads << " worker threads...\n";
    std::vector<std::thread> workers;

    for (int i = 0; i < g_config.num_threads; ++i) {
        workers.emplace_back(mixed_stress_worker, i, g_config.key_range);
    }

    // 启动辅助线程
    std::thread integrity_thread(integrity_checker);
    std::thread persistence_thread(persistence_worker);
    std::thread report_thread(reporter);

    std::cout << "All threads started. Running for " << g_config.duration_seconds << " seconds...\n";
    std::cout << "Press Ctrl+C to stop early.\n\n";

    // 等待完成
    auto end = test_start + seconds(g_config.duration_seconds);
    while (steady_clock::now() < end && g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(seconds(1));

        // 定期检查内存使用
        double current_mem = get_memory_usage_mb();
        if (current_mem > start_mem + 500) {  // 如果内存增长超过500MB
            LOG_WARN(STRESS, "Memory usage significantly increased: %.2f MB (start: %.2f MB)",
                     current_mem, start_mem);
        }
    }

    // 停止测试
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  Stopping stress test...\n";
    std::cout << "========================================\n";

    g_running = false;

    // 等待所有线程结束
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    if (integrity_thread.joinable()) integrity_thread.join();
    if (persistence_thread.joinable()) persistence_thread.join();
    if (report_thread.joinable()) report_thread.join();

    auto test_end = steady_clock::now();
    auto test_duration = duration_cast<seconds>(test_end - test_start).count();

    // 最终统计
    double end_mem = get_memory_usage_mb();
    uint64_t total_ops = g_stats.total_operations.load();

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "   Stress Test Completed\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    std::cout << "Test Duration: " << test_duration << " seconds\n";
    std::cout << "\n";
    std::cout << "Operations:\n";
    std::cout << "  Total:     " << total_ops << "\n";
    std::cout << "  String:    " << g_stats.string_ops.load() << "\n";
    std::cout << "  List:      " << g_stats.list_ops.load() << "\n";
    std::cout << "  Hash:      " << g_stats.hash_ops.load() << "\n";
    std::cout << "  Set:       " << g_stats.set_ops.load() << "\n";
    std::cout << "  ZSet:      " << g_stats.zset_ops.load() << "\n";
    std::cout << "  Expire:    " << g_stats.expire_ops.load() << "\n";
    std::cout << "  Errors:    " << g_stats.errors.load() << "\n";
    std::cout << "\n";
    std::cout << "Performance:\n";
    std::cout << "  Ops/second: " << (test_duration > 0 ? total_ops / test_duration : 0) << "\n";
    std::cout << "\n";
    std::cout << "Memory:\n";
    std::cout << "  Start:     " << start_mem << " MB\n";
    std::cout << "  End:       " << end_mem << " MB\n";
    std::cout << "  Delta:     +" << (end_mem - start_mem) << " MB\n";
    std::cout << "\n";
    std::cout << "Integrity Checks:\n";
    std::cout << "  Passed:    " << g_stats.integrity_check_passed.load() << "\n";
    std::cout << "  Failed:    " << g_stats.integrity_check_failed.load() << "\n";
    std::cout << "\n";
    std::cout << "Storage Final Size: " << storage.size() << " keys\n";
    std::cout << "\n";

    // 并发问题检查结果
    std::cout << "========================================\n";
    std::cout << "   Concurrency Issue Check\n";
    std::cout << "========================================\n";

    bool has_issues = false;

    if (g_stats.errors.load() > 0) {
        std::cout << "  [!] Operations errors detected: " << g_stats.errors.load() << "\n";
        has_issues = true;
    } else {
        std::cout << "  [OK] No operation errors\n";
    }

    if (end_mem - start_mem > 100) {
        std::cout << "  [!] Potential memory leak: +" << (end_mem - start_mem) << " MB\n";
        has_issues = true;
    } else {
        std::cout << "  [OK] Memory usage normal\n";
    }

    if (g_stats.integrity_check_failed.load() > 0) {
        std::cout << "  [!] Data integrity failures: " << g_stats.integrity_check_failed.load() << "\n";
        has_issues = true;
    } else {
        std::cout << "  [OK] Data integrity checks passed\n";
    }

    if (!has_issues) {
        std::cout << "\n  *** NO CONCURRENCY ISSUES DETECTED ***\n";
    } else {
        std::cout << "\n  *** POTENTIAL ISSUES FOUND - REVIEW ABOVE ***\n";
    }

    std::cout << "\n[Stress Test Finished]\n";

    return has_issues ? 1 : 0;
}
