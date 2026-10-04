#include <iostream>
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include "../trace/test_assertions.h"
#include "base/log.h"
#include "cache/storage.h"
#include "datatype/object.h"

namespace cc_server {
namespace testing {

// ============================================================================
// GlobalStorage 基本操作测试
// ============================================================================

void test_storage_basic_operations() {
    TEST_SUITE("GlobalStorage Basic Operations");

    GlobalStorage storage;

    // 测试 SET
    CacheObject obj;
    obj.set_string("test_value");
    storage.set("test_key", obj);

    // 测试 GET
    auto result = storage.get("test_key");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->type(), ObjectType::STRING);
    EXPECT_EQ(result->get_string().value(), std::string("test_value"));

    // 测试 EXISTS
    EXPECT_TRUE(storage.exist("test_key"));
    EXPECT_FALSE(storage.exist("nonexistent_key"));

    // 测试 DEL
    bool deleted = storage.del("test_key");
    EXPECT_TRUE(deleted);
    EXPECT_FALSE(storage.exist("test_key"));

    deleted = storage.del("nonexistent_key");
    EXPECT_FALSE(deleted);

    // 测试 SIZE
    EXPECT_FALSE(storage.exist("test_key"));
    storage.set("key1", obj);
    storage.set("key2", obj);
    storage.set("key3", obj);
    EXPECT_EQ(storage.size(), size_t(3));

    std::cout << "✓ Storage basic operations test passed\n";
}

// ============================================================================
// TTL 功能测试
// ============================================================================

void test_storage_ttl() {
    TEST_SUITE("GlobalStorage TTL");

    GlobalStorage storage;

    CacheObject obj;
    obj.set_string("value_with_ttl");

    // 测试 set_expire (相对时间)
    storage.set("key1", obj);
    storage.set_expire("key1", 2000);  // 2000 毫秒后过期

    EXPECT_TRUE(storage.exist("key1"));
    EXPECT_FALSE(storage.is_expired("key1"));

    // 等待过期
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_TRUE(storage.is_expired("key1"));

    // 过期的键应该无法获取
    auto result = storage.get("key1");
    EXPECT_FALSE(result.has_value());

    std::cout << "✓ Storage TTL test passed\n";
}

void test_storage_set_expire_time() {
    TEST_SUITE("GlobalStorage Set Expire Time");

    GlobalStorage storage;

    CacheObject obj;
    obj.set_string("value");

    // 测试 set_expire_time (绝对时间)
    storage.set("key1", obj);
    auto expire_time = std::chrono::system_clock::now() + std::chrono::seconds(2);
    auto expire_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        expire_time.time_since_epoch()).count();
    storage.expire_dict().set_expire_time("key1", expire_time_ms);

    EXPECT_TRUE(storage.exist("key1"));
    EXPECT_FALSE(storage.is_expired("key1"));

    // 等待过期
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_TRUE(storage.is_expired("key1"));

    std::cout << "✓ Storage set expire time test passed\n";
}

void test_storage_set_with_expire() {
    TEST_SUITE("GlobalStorage Set With Expire");

    GlobalStorage storage;

    CacheObject obj;
    obj.set_string("value");

    // 原子性设置值和过期时间
    storage.set_with_expire("key1", obj, 2000);  // 2000 毫秒

    EXPECT_TRUE(storage.exist("key1"));
    auto result = storage.get("key1");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->get_string().value(), std::string("value"));

    // 等待过期
    std::this_thread::sleep_for(std::chrono::seconds(3));
    result = storage.get("key1");
    EXPECT_FALSE(result.has_value());

    std::cout << "✓ Storage set with expire test passed\n";
}

void test_storage_persist() {
    TEST_SUITE("GlobalStorage Persist");

    GlobalStorage storage;

    CacheObject obj;
    obj.set_string("value");

    // 设置带 TTL 的键
    storage.set_with_expire("key1", obj, 10000);  // 10000 毫秒
    EXPECT_FALSE(storage.is_expired("key1"));

    // 移除 TTL
    bool persisted = storage.expire_dict().persist("key1");
    EXPECT_TRUE(persisted);

    // 等待原本的过期时间
    std::this_thread::sleep_for(std::chrono::seconds(11));

    // 键应该仍然存在
    EXPECT_FALSE(storage.is_expired("key1"));
    auto result = storage.get("key1");
    EXPECT_TRUE(result.has_value());

    std::cout << "✓ Storage persist test passed\n";
}

// ============================================================================
// 极大 TTL 不能溢出成"立刻就过期"
// ============================================================================

void test_storage_huge_ttl_does_not_overflow() {
    TEST_SUITE("GlobalStorage Huge TTL");

    GlobalStorage storage;
    CacheObject obj;
    obj.set_string("survive");

    // EXPIRE/SETEX 传极大的秒数时，用户要的是"实际上永不过期"。
    // 溢出会把它变成负数绝对时间戳，键在下一次读就被惰性删除。
    storage.set_with_expire("huge1", obj, std::numeric_limits<int64_t>::max());
    EXPECT_FALSE(storage.is_expired("huge1"));
    EXPECT_TRUE(storage.exist("huge1"));
    EXPECT_TRUE(storage.get("huge1").has_value());

    // 一个仍然很大但不会触发饱和的 TTL，确认常规路径没被改动带坏
    storage.set_with_expire("big2", obj, 3600LL * 1000 * 24 * 365);  // 一年
    EXPECT_FALSE(storage.is_expired("big2"));
    EXPECT_TRUE(storage.get("big2").has_value());
    const int64_t ttl = storage.expire_dict().get_ttl("big2");
    EXPECT_TRUE(ttl > 3600LL * 1000 * 24 * 364);

    std::cout << "✓ Storage huge TTL saturation test passed\n";
}

// ============================================================================
// 分片内原子读-改-写
// ============================================================================

// mutate() 存在的全部理由：容器命令原来是 get() 拿一份副本、改完 set() 整体覆盖，
// 两个并发命令各自读到旧副本，后写的那个把先写的整个覆盖掉。CI 上
// concurrent_lpush_loses_nothing 的实测就是 8 线程 × 50 次 LPUSH 只剩 212/400。
void test_storage_concurrent_container_mutate() {
    TEST_SUITE("GlobalStorage Atomic Mutate");

    GlobalStorage storage;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&storage, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                storage.mutate("clist", ObjectType::LIST, [&](CacheObject& obj) {
                    obj.list_push(std::to_string(t) + ":" + std::to_string(i), true);
                    return StoreOp::kWrite;
                });
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }

    auto obj = storage.get("clist");
    EXPECT_TRUE(obj.has_value());
    if (obj.has_value()) {
        EXPECT_EQ(obj.value().list_size(), static_cast<size_t>(kThreads * kPerThread));
    }

    // 弹空即删：把 400 个元素全 LPOP 掉之后键必须消失，而不是留下空列表。
    // 多弹几次应当是 kNoop，不能再凭空计数。
    int popped_total = 0;
    for (int i = 0; i < kThreads * kPerThread + 5; ++i) {
        storage.mutate(
            "clist", ObjectType::LIST, [&](CacheObject& o) {
                auto val = o.list_pop(true);
                if (!val) return StoreOp::kNoop;
                ++popped_total;
                return o.list_size() == 0 ? StoreOp::kErase : StoreOp::kWrite;
            },
            StringPromotion::kReject);
    }
    EXPECT_EQ(popped_total, kThreads * kPerThread);
    EXPECT_TRUE(!storage.exist("clist"));

    std::cout << "✓ Storage atomic mutate test passed\n";
}

// ============================================================================
// 脏计数器测试
// ============================================================================

void test_storage_dirty_counter() {
    TEST_SUITE("GlobalStorage Dirty Counter");

    GlobalStorage storage;

    // 初始脏计数应该为 0
    EXPECT_EQ(storage.get_dirty_count(), size_t(0));

    CacheObject obj;
    obj.set_string("value");

    // SET 操作增加脏计数
    storage.set("key1", obj);
    EXPECT_EQ(storage.get_dirty_count(), size_t(1));

    storage.set("key2", obj);
    EXPECT_EQ(storage.get_dirty_count(), size_t(2));

    // DEL 操作增加脏计数
    storage.del("key1");
    EXPECT_EQ(storage.get_dirty_count(), size_t(3));

    // 重置脏计数
    storage.reset_dirty_count();
    EXPECT_EQ(storage.get_dirty_count(), size_t(0));

    // 再次操作
    storage.set("key3", obj);
    EXPECT_EQ(storage.get_dirty_count(), size_t(1));

    std::cout << "✓ Storage dirty counter test passed\n";
}

// ============================================================================
// 并发读写测试
// ============================================================================

void test_storage_concurrent_read_write() {
    TEST_SUITE("GlobalStorage Concurrent Read Write");

    GlobalStorage storage;

    CacheObject obj;
    obj.set_string("initial_value");
    storage.set("shared_key", obj);

    const int num_threads = 8;
    const int operations_per_thread = 1000;
    std::vector<std::thread> threads;

    // 启动多个读写线程
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&storage, i, operations_per_thread]() {
            for (int j = 0; j < operations_per_thread; ++j) {
                // 写操作
                CacheObject obj;
                obj.set_string("value_" + std::to_string(i) + "_" + std::to_string(j));
                storage.set("key_" + std::to_string(i), obj);

                // 读操作
                auto result = storage.get("key_" + std::to_string(i));
                EXPECT_TRUE(result.has_value());
            }
        });
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 验证数据一致性
    EXPECT_EQ(storage.size(), size_t(num_threads + 1));  // +1 for shared_key

    std::cout << "✓ Storage concurrent read write test passed\n";
}

// ============================================================================
// 分片锁性能测试
// ============================================================================

void test_storage_sharding_performance() {
    TEST_SUITE("GlobalStorage Sharding Performance");

    GlobalStorage storage;

    const int num_keys = 10000;
    const int num_threads = 8;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&storage, t, num_keys, num_threads]() {
            int start_key = t * (num_keys / num_threads);
            int end_key = (t + 1) * (num_keys / num_threads);

            for (int i = start_key; i < end_key; ++i) {
                CacheObject obj;
                obj.set_string("value_" + std::to_string(i));
                storage.set("key_" + std::to_string(i), obj);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_EQ(storage.size(), size_t(num_keys));
    std::cout << "  Inserted " << num_keys << " keys with " << num_threads
              << " threads in " << duration << " ms\n";

    std::cout << "✓ Storage sharding performance test passed\n";
}

// ============================================================================
// 大规模数据测试
// ============================================================================

void test_storage_large_dataset() {
    TEST_SUITE("GlobalStorage Large Dataset");

    GlobalStorage storage;

    const int num_keys = 100000;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_keys; ++i) {
        CacheObject obj;
        obj.set_string("value_" + std::to_string(i));
        storage.set("key_" + std::to_string(i), obj);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_EQ(storage.size(), size_t(num_keys));
    std::cout << "  Inserted " << num_keys << " keys in " << duration << " ms\n";

    // 随机读取测试
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        int key_idx = rand() % num_keys;
        auto result = storage.get("key_" + std::to_string(key_idx));
        EXPECT_TRUE(result.has_value());
    }
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "  Random read 10000 keys in " << duration << " ms\n";

    std::cout << "✓ Storage large dataset test passed\n";
}

// ============================================================================
// 不同数据类型存储测试
// ============================================================================

void test_storage_multiple_datatypes() {
    TEST_SUITE("GlobalStorage Multiple DataTypes");

    GlobalStorage storage;

    // String
    CacheObject str_obj;
    str_obj.set_string("string_value");
    storage.set("string_key", str_obj);

    // List
    CacheObject list_obj;
    list_obj.list_push("item1", false);
    list_obj.list_push("item2", false);
    storage.set("list_key", list_obj);

    // Hash
    CacheObject hash_obj;
    hash_obj.hash_set("field1", "value1");
    storage.set("hash_key", hash_obj);

    // Set
    CacheObject set_obj;
    set_obj.set_add("member1");
    storage.set("set_key", set_obj);

    // ZSet
    CacheObject zset_obj;
    zset_obj.zset_add("member1", 1.0);
    storage.set("zset_key", zset_obj);

    // 验证所有类型
    EXPECT_EQ(storage.size(), size_t(5));

    auto str = storage.get("string_key");
    EXPECT_TRUE(str.has_value());
    EXPECT_EQ(str->type(), ObjectType::STRING);

    auto list = storage.get("list_key");
    EXPECT_TRUE(list.has_value());
    EXPECT_EQ(list->type(), ObjectType::LIST);

    auto hash = storage.get("hash_key");
    EXPECT_TRUE(hash.has_value());
    EXPECT_EQ(hash->type(), ObjectType::HASH);

    auto set = storage.get("set_key");
    EXPECT_TRUE(set.has_value());
    EXPECT_EQ(set->type(), ObjectType::SET);

    auto zset = storage.get("zset_key");
    EXPECT_TRUE(zset.has_value());
    EXPECT_EQ(zset->type(), ObjectType::ZSET);

    std::cout << "✓ Storage multiple datatypes test passed\n";
}

// ============================================================================
// 获取所有对象测试
// ============================================================================

void test_storage_get_all_objects() {
    TEST_SUITE("GlobalStorage Get All Objects");

    GlobalStorage storage;

    // 添加一些数据
    for (int i = 0; i < 10; ++i) {
        CacheObject obj;
        obj.set_string("value_" + std::to_string(i));
        storage.set("key_" + std::to_string(i), obj);
    }

    // 获取所有对象
    auto all_objects = storage.get_all_objects_with_ttl();
    EXPECT_EQ(all_objects.size(), size_t(10));

    // 验证数据
    for (const auto& [key, obj, ttl] : all_objects) {
        EXPECT_TRUE(key.find("key_") == 0);
        EXPECT_EQ(obj.type(), ObjectType::STRING);
    }

    std::cout << "✓ Storage get all objects test passed\n";
}

// ============================================================================
// 主测试函数
// ============================================================================

// ============================================================================
// FileSink 建目录不能走 shell
// ============================================================================

// 旧实现把配置里的日志目录拼进 system("mkdir -p " + dir)。目录名来自 conf 的
// log_file，路径里一个分号就变成两条 shell 命令 —— 能改配置文件的人就能以服务器
// 权限执行任意命令。这里用"注入是否会真的执行"来钉死它。
//
// 放在这个二进制里是因为它是唯一链接 base/log.cpp 又不需要起服务器的测试目标。
void test_log_sink_path_is_not_a_shell_command() {
    TEST_SUITE("FileSink Path Is Not A Shell Command");

    const std::string base = "/tmp/cc_sink_probe";
    const std::string marker = base + "_pwned";
    std::remove(marker.c_str());

    // 整个字符串是一个目录名（分号对文件系统来说只是普通字符）
    const std::string weird_dir = base + "; touch " + marker;
    const std::string file = weird_dir + "/app.log";

    {
        FileSink sink(file, 1024 * 1024, 3);
        sink.write("hello");
        sink.flush();
    }

    struct stat st{};
    // 被注入的文件不该存在：说明没有任何 shell 参与
    EXPECT_TRUE(::stat(marker.c_str(), &st) != 0);
    // 而目录本身必须按字面建出来，日志才写得进去
    EXPECT_TRUE(::stat(weird_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

    std::remove(file.c_str());
    ::rmdir(weird_dir.c_str());
    std::remove(marker.c_str());
}

// log_level 的两种词法：conf 里是数字，热加载回调以前只认名字。
// 两边读同一个键，就必须用同一个解析器，且认不出来要说出来而不是默默沿用旧值。
void test_log_level_parsing() {
    TEST_SUITE("log_level 词法");

    LogLevel lvl = LogLevel::INFO;
    EXPECT_TRUE(parse_log_level("4", lvl));
    EXPECT_TRUE(lvl == LogLevel::ERROR);
    EXPECT_TRUE(parse_log_level("0", lvl));
    EXPECT_TRUE(lvl == LogLevel::TRACE);
    EXPECT_TRUE(parse_log_level("5", lvl));
    EXPECT_TRUE(lvl == LogLevel::FATAL);

    EXPECT_TRUE(parse_log_level("info", lvl));
    EXPECT_TRUE(lvl == LogLevel::INFO);
    EXPECT_TRUE(parse_log_level(" WARN ", lvl));   // 大小写与空白
    EXPECT_TRUE(lvl == LogLevel::WARN);

    // 认不出来的必须报 false，而不是悄悄退回某个默认值
    EXPECT_TRUE(!parse_log_level("9", lvl));       // 越界
    EXPECT_TRUE(!parse_log_level("3x", lvl));      // 半截数字
    EXPECT_TRUE(!parse_log_level("verbose", lvl)); // 不是这六个名字
    EXPECT_TRUE(!parse_log_level("", lvl));
}

// BGSAVE 的脏计数按快照扣减，不能把窗口期的写一起抹掉。
//
// 调度线程是"起线程 → 立刻记账"，所以 snapshot 之后还会继续有写进来。旧代码在这里
// 直接 reset 成 0：那批写既没进这次快照，又丢了计数，于是永远攒不到下一次触发。
void test_storage_consume_dirty_count_keeps_window_writes() {
    TEST_SUITE("GlobalStorage Dirty Snapshot");

    GlobalStorage storage;
    CacheObject obj;
    obj.set_string("v");

    storage.reset_dirty_count();
    storage.set("a", obj);
    storage.set("b", obj);
    EXPECT_EQ(storage.get_dirty_count(), static_cast<size_t>(2));

    // 调度线程抓到快照 2，随后 BGSAVE 开始写文件；这期间又来了两笔写
    const size_t snapshot = storage.get_dirty_count();
    storage.set("c", obj);
    storage.set("d", obj);
    EXPECT_EQ(storage.get_dirty_count(), static_cast<size_t>(4));

    storage.consume_dirty_count(snapshot);
    // c、d 不在快照里，必须仍然算脏（旧实现会把计数变成 0）
    EXPECT_EQ(storage.get_dirty_count(), static_cast<size_t>(2));

    // 扣得比现存的多时饱和到 0，不能下溢成巨大的 size_t —— 那会让每一轮都过阈值
    storage.consume_dirty_count(1000);
    EXPECT_EQ(storage.get_dirty_count(), static_cast<size_t>(0));
    EXPECT_TRUE(storage.get_dirty_count() < static_cast<size_t>(100));
}

void run_all_storage_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running GlobalStorage V3 Tests\n";
    std::cout << "========================================\n\n";

    test_storage_basic_operations();
    test_storage_ttl();
    test_storage_set_expire_time();
    test_storage_set_with_expire();
    test_storage_persist();
    test_storage_huge_ttl_does_not_overflow();
    test_storage_concurrent_container_mutate();
    test_storage_dirty_counter();
    test_storage_consume_dirty_count_keeps_window_writes();
    test_storage_concurrent_read_write();
    test_storage_sharding_performance();
    test_storage_large_dataset();
    test_storage_multiple_datatypes();
    test_storage_get_all_objects();
    test_log_sink_path_is_not_a_shell_command();
    test_log_level_parsing();

    std::cout << "\n========================================\n";
    std::cout << "All GlobalStorage V3 Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
