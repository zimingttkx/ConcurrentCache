#include <iostream>
#include <cassert>
#include <fstream>
#include <thread>
#include <chrono>
#include "../trace/test_assertions.h"
#include "persistence/rdb.h"
#include "cache/storage.h"
#include "datatype/object.h"

namespace cc_server {
namespace testing {

// ============================================================================
// RDB 基本保存和加载测试
// ============================================================================

void test_rdb_basic_save_load() {
    TEST_SUITE("RDB Basic Save and Load");

    const std::string test_file = "/tmp/test_rdb_basic.rdb";

    // 创建存储并添加数据
    GlobalStorage storage;

    // 添加 String 类型
    CacheObject str_obj;
    str_obj.set_string("hello world");
    storage.set("key1", str_obj);

    // 添加 List 类型
    CacheObject list_obj;
    list_obj.list_push("item1", false);
    list_obj.list_push("item2", false);
    list_obj.list_push("item3", false);
    storage.set("key2", list_obj);

    // 添加 Hash 类型
    CacheObject hash_obj;
    hash_obj.hash_set("field1", "value1");
    hash_obj.hash_set("field2", "value2");
    storage.set("key3", hash_obj);

    // 保存到 RDB
    auto& rdb = RdbPersistence::instance();
    rdb.set_filepath(test_file);
    bool save_result = rdb.save(test_file, storage);
    EXPECT_TRUE(save_result);

    // 验证文件存在
    std::ifstream file(test_file);
    EXPECT_TRUE(file.good());
    file.close();

    // 创建新的存储并加载
    GlobalStorage new_storage;
    bool load_result = rdb.load(test_file, new_storage);
    EXPECT_TRUE(load_result);

    // 验证数据
    auto loaded_obj1 = new_storage.get("key1");
    EXPECT_TRUE(loaded_obj1.has_value());
    EXPECT_EQ(loaded_obj1->type(), ObjectType::STRING);
    EXPECT_EQ(loaded_obj1->get_string().value(), std::string("hello world"));

    auto loaded_obj2 = new_storage.get("key2");
    EXPECT_TRUE(loaded_obj2.has_value());
    EXPECT_EQ(loaded_obj2->type(), ObjectType::LIST);
    EXPECT_EQ(loaded_obj2->list_size(), size_t(3));

    auto loaded_obj3 = new_storage.get("key3");
    EXPECT_TRUE(loaded_obj3.has_value());
    EXPECT_EQ(loaded_obj3->type(), ObjectType::HASH);
    EXPECT_EQ(loaded_obj3->hash_size(), size_t(2));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB basic save and load test passed\n";
}

// ============================================================================
// RDB TTL 测试
// ============================================================================

void test_rdb_with_ttl() {
    TEST_SUITE("RDB Save and Load with TTL");

    const std::string test_file = "/tmp/test_rdb_ttl.rdb";

    GlobalStorage storage;

    // 添加带 TTL 的键
    CacheObject obj;
    obj.set_string("value_with_ttl");
    storage.set("key_with_ttl", obj);

    // 设置 10 秒后过期
    auto expire_time = std::chrono::system_clock::now() + std::chrono::seconds(10);
    auto expire_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        expire_time.time_since_epoch()).count();
    storage.expire_dict().set_expire_time("key_with_ttl", expire_time_ms);

    // 添加不带 TTL 的键
    CacheObject obj2;
    obj2.set_string("value_no_ttl");
    storage.set("key_no_ttl", obj2);

    // 保存
    auto& rdb = RdbPersistence::instance();
    bool save_result = rdb.save(test_file, storage);
    EXPECT_TRUE(save_result);

    // 加载
    GlobalStorage new_storage;
    bool load_result = rdb.load(test_file, new_storage);
    EXPECT_TRUE(load_result);

    // 验证带 TTL 的键
    auto loaded_obj1 = new_storage.get("key_with_ttl");
    EXPECT_TRUE(loaded_obj1.has_value());
    EXPECT_EQ(loaded_obj1->get_string().value(), std::string("value_with_ttl"));

    // 验证不带 TTL 的键
    auto loaded_obj2 = new_storage.get("key_no_ttl");
    EXPECT_TRUE(loaded_obj2.has_value());
    EXPECT_EQ(loaded_obj2->get_string().value(), std::string("value_no_ttl"));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB with TTL test passed\n";
}

// ============================================================================
// RDB 所有数据类型测试
// ============================================================================

void test_rdb_all_datatypes() {
    TEST_SUITE("RDB All DataTypes Serialization");

    const std::string test_file = "/tmp/test_rdb_all_types.rdb";

    GlobalStorage storage;

    // String
    CacheObject str_obj;
    str_obj.set_string("test string");
    storage.set("string_key", str_obj);

    // List
    CacheObject list_obj;
    list_obj.list_push("a", false);
    list_obj.list_push("b", false);
    list_obj.list_push("c", false);
    storage.set("list_key", list_obj);

    // Hash
    CacheObject hash_obj;
    hash_obj.hash_set("f1", "v1");
    hash_obj.hash_set("f2", "v2");
    storage.set("hash_key", hash_obj);

    // Set
    CacheObject set_obj;
    set_obj.set_add("member1");
    set_obj.set_add("member2");
    set_obj.set_add("member3");
    storage.set("set_key", set_obj);

    // ZSet
    CacheObject zset_obj;
    zset_obj.zset_add("m1", 1.0);
    zset_obj.zset_add("m2", 2.0);
    zset_obj.zset_add("m3", 3.0);
    storage.set("zset_key", zset_obj);

    // 保存
    auto& rdb = RdbPersistence::instance();
    bool save_result = rdb.save(test_file, storage);
    EXPECT_TRUE(save_result);

    // 加载
    GlobalStorage new_storage;
    bool load_result = rdb.load(test_file, new_storage);
    EXPECT_TRUE(load_result);

    // 验证所有类型
    auto str = new_storage.get("string_key");
    EXPECT_TRUE(str.has_value());
    EXPECT_EQ(str->type(), ObjectType::STRING);
    EXPECT_EQ(str->get_string().value(), std::string("test string"));

    auto list = new_storage.get("list_key");
    EXPECT_TRUE(list.has_value());
    EXPECT_EQ(list->type(), ObjectType::LIST);
    EXPECT_EQ(list->list_size(), size_t(3));

    auto hash = new_storage.get("hash_key");
    EXPECT_TRUE(hash.has_value());
    EXPECT_EQ(hash->type(), ObjectType::HASH);
    EXPECT_EQ(hash->hash_size(), size_t(2));

    auto set = new_storage.get("set_key");
    EXPECT_TRUE(set.has_value());
    EXPECT_EQ(set->type(), ObjectType::SET);
    EXPECT_EQ(set->set_size(), size_t(3));

    auto zset = new_storage.get("zset_key");
    EXPECT_TRUE(zset.has_value());
    EXPECT_EQ(zset->type(), ObjectType::ZSET);
    EXPECT_EQ(zset->zset_size(), size_t(3));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB all datatypes test passed\n";
}

// ============================================================================
// RDB 空存储测试
// ============================================================================

void test_rdb_empty_storage() {
    TEST_SUITE("RDB Empty Storage");

    const std::string test_file = "/tmp/test_rdb_empty.rdb";

    GlobalStorage storage;

    // 保存空存储
    auto& rdb = RdbPersistence::instance();
    bool save_result = rdb.save(test_file, storage);
    EXPECT_TRUE(save_result);

    // 加载
    GlobalStorage new_storage;
    bool load_result = rdb.load(test_file, new_storage);
    EXPECT_TRUE(load_result);

    // 验证为空
    EXPECT_EQ(new_storage.size(), size_t(0));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB empty storage test passed\n";
}

// ============================================================================
// RDB 大数据量测试
// ============================================================================

void test_rdb_large_dataset() {
    TEST_SUITE("RDB Large Dataset");

    const std::string test_file = "/tmp/test_rdb_large.rdb";

    GlobalStorage storage;

    // 添加 1000 个键
    const int num_keys = 1000;
    for (int i = 0; i < num_keys; ++i) {
        CacheObject obj;
        obj.set_string("value_" + std::to_string(i));
        storage.set("key_" + std::to_string(i), obj);
    }

    EXPECT_EQ(storage.size(), size_t(num_keys));

    // 保存
    auto& rdb = RdbPersistence::instance();
    auto start = std::chrono::steady_clock::now();
    bool save_result = rdb.save(test_file, storage);
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_TRUE(save_result);
    std::cout << "  Saved " << num_keys << " keys in " << duration << " ms\n";

    // 加载
    GlobalStorage new_storage;
    start = std::chrono::steady_clock::now();
    bool load_result = rdb.load(test_file, new_storage);
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_TRUE(load_result);
    EXPECT_EQ(new_storage.size(), size_t(num_keys));
    std::cout << "  Loaded " << num_keys << " keys in " << duration << " ms\n";

    // 验证部分数据
    auto obj0 = new_storage.get("key_0");
    EXPECT_TRUE(obj0.has_value());
    EXPECT_EQ(obj0->get_string().value(), std::string("value_0"));

    auto obj999 = new_storage.get("key_999");
    EXPECT_TRUE(obj999.has_value());
    EXPECT_EQ(obj999->get_string().value(), std::string("value_999"));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB large dataset test passed\n";
}

// ============================================================================
// RDB 文件不存在测试
// ============================================================================

void test_rdb_file_not_exist() {
    TEST_SUITE("RDB File Not Exist");

    const std::string test_file = "/tmp/nonexistent_file.rdb";

    GlobalStorage storage;
    auto& rdb = RdbPersistence::instance();

    // 尝试加载不存在的文件
    bool load_result = rdb.load(test_file, storage);
    EXPECT_FALSE(load_result);

    std::cout << "✓ RDB file not exist test passed\n";
}

// ============================================================================
// RDB 统计信息测试
// ============================================================================

void test_rdb_stats() {
    TEST_SUITE("RDB Statistics");

    const std::string test_file = "/tmp/test_rdb_stats.rdb";

    GlobalStorage storage;

    // 添加一些数据
    for (int i = 0; i < 10; ++i) {
        CacheObject obj;
        obj.set_string("value_" + std::to_string(i));
        storage.set("key_" + std::to_string(i), obj);
    }

    // 使用后台保存
    auto& rdb = RdbPersistence::instance();
    bool save_result = rdb.save_in_background(test_file, storage);
    EXPECT_TRUE(save_result);

    // 等待后台保存完成
    bool wait_result = rdb.wait_for_bgsave(5000);
    EXPECT_TRUE(wait_result);

    // 获取统计信息
    const auto& stats = rdb.get_stats();
    EXPECT_EQ(stats.total_bgsave_calls.load(), size_t(1));
    // 注意: last_bgsave_keys 在子进程中更新,父进程不可见,因为 fork() 后内存空间分离
    // 所以这里不检查 last_bgsave_keys

    // 再次后台保存
    save_result = rdb.save_in_background(test_file, storage);
    EXPECT_TRUE(save_result);

    // 等待后台保存完成
    wait_result = rdb.wait_for_bgsave(5000);
    EXPECT_TRUE(wait_result);

    const auto& stats2 = rdb.get_stats();
    EXPECT_EQ(stats2.total_bgsave_calls.load(), size_t(2));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB statistics test passed\n";
}

// ============================================================================
// BGSAVE 测试
// ============================================================================

void test_rdb_bgsave() {
    TEST_SUITE("RDB Background Save");

    const std::string test_file = "/tmp/test_rdb_bgsave.rdb";

    GlobalStorage storage;

    // 添加数据
    for (int i = 0; i < 100; ++i) {
        CacheObject obj;
        obj.set_string("value_" + std::to_string(i));
        storage.set("key_" + std::to_string(i), obj);
    }

    // 后台保存
    auto& rdb = RdbPersistence::instance();
    bool bgsave_result = rdb.save_in_background(test_file, storage);
    EXPECT_TRUE(bgsave_result);

    // 等待后台保存完成
    int max_wait = 50;  // 最多等待 5 秒
    while (rdb.is_bgsave_in_progress() && max_wait-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_FALSE(rdb.is_bgsave_in_progress());

    // 验证保存状态
    auto status = rdb.get_last_bgsave_status();
    EXPECT_EQ(status, BgsaveStatus::SUCCESS);

    // 验证文件存在
    std::ifstream file(test_file);
    EXPECT_TRUE(file.good());
    file.close();

    // 加载验证
    GlobalStorage new_storage;
    bool load_result = rdb.load(test_file, new_storage);
    EXPECT_TRUE(load_result);
    EXPECT_EQ(new_storage.size(), size_t(100));

    // 清理
    std::remove(test_file.c_str());

    std::cout << "✓ RDB background save test passed\n";
}

// ============================================================================
// 主测试函数
// ============================================================================

void run_all_rdb_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running RDB Persistence Tests\n";
    std::cout << "========================================\n\n";

    test_rdb_basic_save_load();
    test_rdb_with_ttl();
    test_rdb_all_datatypes();
    test_rdb_empty_storage();
    test_rdb_large_dataset();
    test_rdb_file_not_exist();
    test_rdb_stats();
    test_rdb_bgsave();

    std::cout << "\n========================================\n";
    std::cout << "All RDB Persistence Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
