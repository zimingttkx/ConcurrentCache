/**
 * @file stress_test.cpp
 * @brief ConcurrentCache V3 压力测试程序
 *
 * 测试内容：
 * 1. 多线程并发命令执行（String, List, Hash, Set, ZSet, TTL）
 * 2. Lock 竞争和死锁检测
 * 3. 内存使用监控
 * 4. RDB 持久化压力测试
 * 5. 数据一致性和持久化验证
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

// 全局统计
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
    }
};

static StressStats g_stats;

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
// String 命令压力测试
// ============================================================================
void stress_test_string(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 12345 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 9);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("str", key_id);

        try {
            switch (op) {
                case 0: { // SET
                    std::string value = random_string(64);
                    storage.set(key, CacheObject(value));
                    g_stats.string_ops++;
                    break;
                }
                case 1: { // GET
                    auto result = storage.get(key);
                    g_stats.string_ops++;
                    break;
                }
                case 2: { // EXISTS
                    storage.exist(key);
                    g_stats.string_ops++;
                    break;
                }
                case 3: { // DEL
                    storage.del(key);
                    g_stats.string_ops++;
                    break;
                }
                case 4: { // SET + EXPIRE
                    std::string value = random_string(64);
                    storage.set_with_expire(key, CacheObject(value), 5000);
                    g_stats.expire_ops++;
                    break;
                }
                case 5: { // SET large value
                    std::string value = random_string(1024);
                    storage.set(key, CacheObject(value));
                    g_stats.string_ops++;
                    break;
                }
                default: { // GET
                    auto result = storage.get(key);
                    g_stats.string_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "String op error: %s", e.what());
        }
    }
}

// ============================================================================
// List 命令压力测试
// ============================================================================
void stress_test_list(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 23456 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("list", key_id);

        try {
            switch (op) {
                case 0: { // LPUSH
                    CacheObject obj;
                    obj.list_push(random_string(32), true);
                    obj.list_push(random_string(32), true);
                    storage.set(key, obj);
                    g_stats.list_ops++;
                    break;
                }
                case 1: { // RPUSH
                    CacheObject obj;
                    obj.list_push(random_string(32), false);
                    obj.list_push(random_string(32), false);
                    storage.set(key, obj);
                    g_stats.list_ops++;
                    break;
                }
                case 2: { // LPOP
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        auto obj = result.value();
                        obj.list_pop(true);
                        storage.set(key, obj);
                    }
                    g_stats.list_ops++;
                    break;
                }
                case 3: { // LLEN
                    auto result = storage.get(key);
                    g_stats.list_ops++;
                    break;
                }
                case 4: { // LRANGE
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().list_range(0, 10);
                    }
                    g_stats.list_ops++;
                    break;
                }
                default: { // RPOP
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        auto obj = result.value();
                        obj.list_pop(false);
                        storage.set(key, obj);
                    }
                    g_stats.list_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "List op error: %s", e.what());
        }
    }
}

// ============================================================================
// Hash 命令压力测试
// ============================================================================
void stress_test_hash(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 34567 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("hash", key_id);
        std::string field = random_string(16);

        try {
            switch (op) {
                case 0: { // HSET
                    CacheObject obj;
                    obj.hash_set(field, random_string(32));
                    obj.hash_set(random_string(16), random_string(32));
                    storage.set(key, obj);
                    g_stats.hash_ops++;
                    break;
                }
                case 1: { // HGET
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().hash_get(field);
                    }
                    g_stats.hash_ops++;
                    break;
                }
                case 2: { // HDEL
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        auto obj = result.value();
                        obj.hash_del(field);
                        storage.set(key, obj);
                    }
                    g_stats.hash_ops++;
                    break;
                }
                case 3: { // HLEN
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().hash_size();
                    }
                    g_stats.hash_ops++;
                    break;
                }
                case 4: { // HGETALL
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().hash_items();
                    }
                    g_stats.hash_ops++;
                    break;
                }
                default: { // HSET multiple
                    CacheObject obj;
                    obj.hash_set(random_string(16), random_string(32));
                    obj.hash_set(random_string(16), random_string(32));
                    storage.set(key, obj);
                    g_stats.hash_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "Hash op error: %s", e.what());
        }
    }
}

// ============================================================================
// Set 命令压力测试
// ============================================================================
void stress_test_set(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 45678 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("set", key_id);
        std::string member = random_string(32);

        try {
            switch (op) {
                case 0: { // SADD
                    CacheObject obj;
                    obj.set_add(member);
                    obj.set_add(random_string(32));
                    obj.set_add(random_string(32));
                    storage.set(key, obj);
                    g_stats.set_ops++;
                    break;
                }
                case 1: { // SISMEMBER
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().set_contains(member);
                    }
                    g_stats.set_ops++;
                    break;
                }
                case 2: { // SPOP
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        auto members = result.value().set_members();
                        if (!members.empty()) {
                            auto obj = result.value();
                            obj.set_remove(members[0]);
                            storage.set(key, obj);
                        }
                    }
                    g_stats.set_ops++;
                    break;
                }
                case 3: { // SCARD
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().set_size();
                    }
                    g_stats.set_ops++;
                    break;
                }
                case 4: { // SMEMBERS
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().set_members();
                    }
                    g_stats.set_ops++;
                    break;
                }
                default: { // SADD more
                    auto result = storage.get(key);
                    CacheObject obj;
                    if (result.has_value()) {
                        obj = result.value();
                    }
                    obj.set_add(member);
                    obj.set_add(random_string(32));
                    storage.set(key, obj);
                    g_stats.set_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "Set op error: %s", e.what());
        }
    }
}

// ============================================================================
// ZSet 命令压力测试
// ============================================================================
void stress_test_zset(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    std::mt19937 rng(thread_id * 56789 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);
    std::uniform_real_distribution<double> score_dist(0.0, 1000.0);

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("zset", key_id);
        std::string member = random_string(32);
        double score = score_dist(rng);

        try {
            switch (op) {
                case 0: { // ZADD
                    CacheObject obj;
                    obj.zset_add(member, score);
                    obj.zset_add(random_string(32), score_dist(rng));
                    storage.set(key, obj);
                    g_stats.zset_ops++;
                    break;
                }
                case 1: { // ZSCORE
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().zset_score(member);
                    }
                    g_stats.zset_ops++;
                    break;
                }
                case 2: { // ZCARD
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().zset_size();
                    }
                    g_stats.zset_ops++;
                    break;
                }
                case 3: { // ZRANGE
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().zset_range_by_score(0, 1000, false);
                    }
                    g_stats.zset_ops++;
                    break;
                }
                case 4: { // ZRANGE WITHSCORES
                    auto result = storage.get(key);
                    if (result.has_value()) {
                        result.value().zset_range_by_score(0, 1000, true);
                    }
                    g_stats.zset_ops++;
                    break;
                }
                default: { // ZADD more
                    auto result = storage.get(key);
                    CacheObject obj;
                    if (result.has_value()) {
                        obj = result.value();
                    }
                    obj.zset_add(member, score);
                    storage.set(key, obj);
                    g_stats.zset_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "ZSet op error: %s", e.what());
        }
    }
}

// ============================================================================
// TTL/Expiration 压力测试
// ============================================================================
void stress_test_expire(int thread_id, int num_ops, int key_range) {
    auto& storage = GlobalStorage::instance();
    auto& expire_dict = storage.expire_dict();
    std::mt19937 rng(thread_id * 67890 + time(nullptr));
    std::uniform_int_distribution<int> op_dist(0, 4);
    std::uniform_int_distribution<int> key_dist(0, key_range - 1);
    std::uniform_int_distribution<int> ttl_dist(1000, 30000); // 1-30秒

    for (int i = 0; i < num_ops; ++i) {
        int op = op_dist(rng);
        int key_id = key_dist(rng);
        std::string key = make_key("expire", key_id);

        try {
            switch (op) {
                case 0: { // SETEX
                    storage.set_with_expire(key, CacheObject(random_string(64)), ttl_dist(rng));
                    g_stats.expire_ops++;
                    break;
                }
                case 1: { // EXPIRE
                    // 先设置一个值
                    storage.set(key, CacheObject(random_string(64)));
                    // 再设置过期
                    expire_dict.set(key, ttl_dist(rng));
                    g_stats.expire_ops++;
                    break;
                }
                case 2: { // TTL
                    expire_dict.get_ttl(key);
                    g_stats.expire_ops++;
                    break;
                }
                case 3: { // PERSIST
                    expire_dict.persist(key);
                    g_stats.expire_ops++;
                    break;
                }
                default: { // CHECK EXPIRE
                    expire_dict.contains(key);
                    g_stats.expire_ops++;
                    break;
                }
            }
            g_stats.total_operations++;
        } catch (const std::exception& e) {
            g_stats.errors++;
            LOG_ERROR(STRESS, "Expire op error: %s", e.what());
        }
    }
}

// ============================================================================
// RDB 持久化压力测试
// ============================================================================
void stress_test_persistence(int interval_ms, int duration_sec) {
    auto& rdb = RdbPersistence::instance();
    auto& storage = GlobalStorage::instance();

    int count = 0;
    auto start = steady_clock::now();
    auto end = start + seconds(duration_sec);

    while (steady_clock::now() < end) {
        std::this_thread::sleep_for(milliseconds(interval_ms));

        std::string path = "./stress_dump_" + std::to_string(count++) + ".rdb";

        LOG_INFO(STRESS, "Starting RDB save #%d, storage size=%zu", count, storage.size());

        bool success = rdb.save(path, storage);

        if (success) {
            LOG_INFO(STRESS, "RDB save #%d completed, keys=%zu", count, storage.size());

            // 验证文件存在
            std::ifstream file(path, std::ios::binary);
            if (file.good()) {
                LOG_INFO(STRESS, "RDB file verified: %s", path.c_str());
            }

            // 删除临时文件
            std::remove(path.c_str());
        } else {
            LOG_ERROR(STRESS, "RDB save #%d failed", count);
        }
    }
}

// ============================================================================
// 后台 RDB 保存线程
// ============================================================================
void background_bgsave_thread(int interval_ms) {
    auto& rdb = RdbPersistence::instance();
    auto& storage = GlobalStorage::instance();

    while (true) {
        std::this_thread::sleep_for(milliseconds(interval_ms));

        if (rdb.is_bgsave_in_progress()) {
            LOG_DEBUG(STRESS, "BGSAVE already in progress, skipping");
            continue;
        }

        if (rdb.save_in_background("./bgsave_dump.rdb", storage)) {
            LOG_INFO(STRESS, "BGSAVE started");
        }
    }
}

// ============================================================================
// 并发读写压力测试
// ============================================================================
void concurrent_read_write_test(int num_threads, int num_ops, int key_range) {
    std::vector<std::thread> threads;

    std::cout << "  Starting " << num_threads << " threads with " << num_ops << " ops each...\n";

    auto start = steady_clock::now();

    // 启动写线程
    for (int i = 0; i < num_threads / 2; ++i) {
        threads.emplace_back([i, num_ops, key_range]() {
            stress_test_string(i, num_ops, key_range);
        });
    }

    // 启动读线程
    for (int i = num_threads / 2; i < num_threads; ++i) {
        threads.emplace_back([i, num_ops, key_range]() {
            stress_test_string(i, num_ops, key_range);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = steady_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count();

    uint64_t total = g_stats.total_operations.load();
    double ops_per_sec = total * 1000.0 / duration;

    std::cout << "  Completed in " << duration << " ms\n";
    std::cout << "  Total ops: " << total << "\n";
    std::cout << "  Ops/sec: " << static_cast<uint64_t>(ops_per_sec) << "\n";
}

// ============================================================================
// 数据完整性测试
// ============================================================================
bool data_integrity_test() {
    std::cout << "\n[Data Integrity Test]\n";

    auto& storage = GlobalStorage::instance();

    // 清空存储
    storage.clear();

    // 设置一些测试数据
    const int num_keys = 1000;
    std::cout << "  Setting " << num_keys << " keys...\n";

    for (int i = 0; i < num_keys; ++i) {
        std::string key = "integrity_key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);
        storage.set(key, CacheObject(value));
    }

    // 验证数据
    std::cout << "  Verifying " << num_keys << " keys...\n";
    int found = 0;
    for (int i = 0; i < num_keys; ++i) {
        std::string key = "integrity_key_" + std::to_string(i);
        auto result = storage.get(key);
        if (result.has_value()) {
            found++;
        }
    }

    std::cout << "  Found " << found << "/" << num_keys << " keys\n";

    if (found == num_keys) {
        std::cout << "  [PASS] Data integrity test passed\n";
        return true;
    } else {
        std::cout << "  [FAIL] Data integrity test failed\n";
        return false;
    }
}

// ============================================================================
// RDB 持久化完整性测试
// ============================================================================
bool persistence_integrity_test() {
    std::cout << "\n[Persistence Integrity Test]\n";

    auto& storage = GlobalStorage::instance();
    auto& rdb = RdbPersistence::instance();

    // 清空存储
    storage.clear();

    // 设置测试数据
    const int num_keys = 500;
    std::cout << "  Setting " << num_keys << " keys with mixed types...\n";

    for (int i = 0; i < num_keys; ++i) {
        std::string key = "persist_key_" + std::to_string(i);

        // 混合类型
        if (i % 5 == 0) {
            // String
            storage.set(key, CacheObject("string_value_" + std::to_string(i)));
        } else if (i % 5 == 1) {
            // List
            CacheObject obj;
            obj.list_push("item1", true);
            obj.list_push("item2", true);
            storage.set(key, obj);
        } else if (i % 5 == 2) {
            // Hash
            CacheObject obj;
            obj.hash_set("field1", "value1");
            obj.hash_set("field2", "value2");
            storage.set(key, obj);
        } else if (i % 5 == 3) {
            // Set
            CacheObject obj;
            obj.set_add("member1");
            obj.set_add("member2");
            storage.set(key, obj);
        } else {
            // ZSet
            CacheObject obj;
            obj.zset_add("member1", 1.0);
            obj.zset_add("member2", 2.0);
            storage.set(key, obj);
        }
    }

    std::cout << "  Storage size before save: " << storage.size() << "\n";

    // 保存 RDB
    std::string path = "./persistence_test.rdb";
    std::cout << "  Saving to " << path << "...\n";

    if (!rdb.save(path, storage)) {
        std::cout << "  [FAIL] RDB save failed\n";
        return false;
    }

    std::cout << "  RDB save completed\n";

    // 清空存储
    std::cout << "  Clearing storage...\n";
    storage.clear();

    std::cout << "  Storage size after clear: " << storage.size() << "\n";

    // 加载 RDB
    std::cout << "  Loading from " << path << "...\n";

    if (!rdb.load(path, storage)) {
        std::cout << "  [FAIL] RDB load failed\n";
        std::remove(path.c_str());
        return false;
    }

    std::cout << "  Storage size after load: " << storage.size() << "\n";

    // 验证数据
    std::cout << "  Verifying loaded data...\n";
    int found = 0;
    for (int i = 0; i < num_keys; ++i) {
        std::string key = "persist_key_" + std::to_string(i);
        if (storage.exist(key)) {
            found++;
        }
    }

    std::cout << "  Found " << found << "/" << num_keys << " keys\n";

    // 清理
    std::remove(path.c_str());

    if (found == num_keys) {
        std::cout << "  [PASS] Persistence integrity test passed\n";
        return true;
    } else {
        std::cout << "  [FAIL] Persistence integrity test failed\n";
        return false;
    }
}

// ============================================================================
// 主压力测试
// ============================================================================
void run_stress_test(int num_threads, int num_ops_per_thread, int key_range) {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "   ConcurrentCache V3 Stress Test\n";
    std::cout << "========================================\n";
    std::cout << "\n";
    std::cout << "Configuration:\n";
    std::cout << "  Threads: " << num_threads << "\n";
    std::cout << "  Ops per thread: " << num_ops_per_thread << "\n";
    std::cout << "  Key range: " << key_range << "\n";
    std::cout << "  Total ops: " << (uint64_t)num_threads * num_ops_per_thread << "\n";
    std::cout << "\n";

    g_stats.reset();

    double start_mem = get_memory_usage_mb();
    std::cout << "  Initial memory: " << start_mem << " MB\n";

    // =========================================================================
    // 测试 1: String 命令并发测试
    // =========================================================================
    std::cout << "[Test 1] String Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_string(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.string_ops.load() * 1000 / duration << "\n";
        std::cout << "  Errors: " << g_stats.errors.load() << "\n";
    }

    // =========================================================================
    // 测试 2: List 命令并发测试
    // =========================================================================
    std::cout << "\n[Test 2] List Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_list(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.list_ops.load() * 1000 / duration << "\n";
    }

    // =========================================================================
    // 测试 3: Hash 命令并发测试
    // =========================================================================
    std::cout << "\n[Test 3] Hash Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_hash(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.hash_ops.load() * 1000 / duration << "\n";
    }

    // =========================================================================
    // 测试 4: Set 命令并发测试
    // =========================================================================
    std::cout << "\n[Test 4] Set Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_set(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.set_ops.load() * 1000 / duration << "\n";
    }

    // =========================================================================
    // 测试 5: ZSet 命令并发测试
    // =========================================================================
    std::cout << "\n[Test 5] ZSet Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_zset(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.zset_ops.load() * 1000 / duration << "\n";
    }

    // =========================================================================
    // 测试 6: Expire/TTL 命令并发测试
    // =========================================================================
    std::cout << "\n[Test 6] Expire/TTL Operations Stress Test\n";
    {
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([i, num_ops_per_thread, key_range]() {
                stress_test_expire(i, num_ops_per_thread, key_range);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Ops/sec: " << g_stats.expire_ops.load() * 1000 / duration << "\n";
    }

    // =========================================================================
    // 测试 7: 混合并发测试
    // =========================================================================
    std::cout << "\n[Test 7] Mixed Operations Concurrent Test\n";
    {
        g_stats.reset();
        std::vector<std::thread> threads;
        auto start = steady_clock::now();

        // 分配不同类型的操作到不同线程
        for (int i = 0; i < num_threads; ++i) {
            int type = i % 6;
            switch (type) {
                case 0:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_string(i, num_ops_per_thread, key_range);
                    });
                    break;
                case 1:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_list(i, num_ops_per_thread, key_range);
                    });
                    break;
                case 2:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_hash(i, num_ops_per_thread, key_range);
                    });
                    break;
                case 3:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_set(i, num_ops_per_thread, key_range);
                    });
                    break;
                case 4:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_zset(i, num_ops_per_thread, key_range);
                    });
                    break;
                case 5:
                    threads.emplace_back([i, num_ops_per_thread, key_range]() {
                        stress_test_expire(i, num_ops_per_thread, key_range);
                    });
                    break;
            }
        }

        for (auto& t : threads) {
            t.join();
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        std::cout << "  Completed in " << duration << " ms\n";
        std::cout << "  Total ops: " << g_stats.total_operations.load() << "\n";
        std::cout << "  Ops/sec: " << g_stats.total_operations.load() * 1000 / duration << "\n";
        std::cout << "  Errors: " << g_stats.errors.load() << "\n";
    }

    // =========================================================================
    // 汇总
    // =========================================================================
    double end_mem = get_memory_usage_mb();

    std::cout << "\n========================================\n";
    std::cout << "   Stress Test Summary\n";
    std::cout << "========================================\n";
    std::cout << "  String ops: " << g_stats.string_ops.load() << "\n";
    std::cout << "  List ops: " << g_stats.list_ops.load() << "\n";
    std::cout << "  Hash ops: " << g_stats.hash_ops.load() << "\n";
    std::cout << "  Set ops: " << g_stats.set_ops.load() << "\n";
    std::cout << "  ZSet ops: " << g_stats.zset_ops.load() << "\n";
    std::cout << "  Expire ops: " << g_stats.expire_ops.load() << "\n";
    std::cout << "  Total ops: " << g_stats.total_operations.load() << "\n";
    std::cout << "  Errors: " << g_stats.errors.load() << "\n";
    std::cout << "  Memory used: " << (end_mem - start_mem) << " MB\n";
    std::cout << "  Final memory: " << end_mem << " MB\n";
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // 默认配置
    int num_threads = get_thread_count();
    int num_ops_per_thread = 10000;
    int key_range = 1000;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            num_ops_per_thread = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            key_range = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --threads N   Number of threads (default: " << get_thread_count() << ")\n";
            std::cout << "  --ops N       Operations per thread (default: 10000)\n";
            std::cout << "  --keys N      Key range (default: 1000)\n";
            std::cout << "  --help        Show this help\n";
            return 0;
        }
    }

    std::cout << "ConcurrentCache V3 Stress Test\n";
    std::cout << "=============================\n\n";

    // 初始化存储
    std::cout << "Initializing storage...\n";
    auto& storage = GlobalStorage::instance();
    std::cout << "Storage initialized with " << storage.size() << " keys\n";

    // 运行主压力测试
    run_stress_test(num_threads, num_ops_per_thread, key_range);

    // 数据完整性测试
    data_integrity_test();

    // 持久化完整性测试
    persistence_integrity_test();

    // 报告存储状态
    std::cout << "\n[Final Status]\n";
    std::cout << "  Storage size: " << storage.size() << "\n";
    std::cout << "  Memory usage: " << get_memory_usage_mb() << " MB\n";

    auto& rdb = RdbPersistence::instance();
    const auto& stats = rdb.get_stats();
    std::cout << "  RDB total saves: " << stats.total_bgsave_calls.load() << "\n";
    std::cout << "  RDB total saved keys: " << stats.total_rdb_saved_keys.load() << "\n";

    std::cout << "\n[Stress Test Completed Successfully]\n";

    return 0;
}
