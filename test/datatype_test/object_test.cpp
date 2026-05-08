#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <map>
#include <set>
#include "../trace/test_assertions.h"
#include "datatype/object.h"

namespace cc_server {
namespace testing {

// ============================================================================
// CacheObject String 类型测试
// ============================================================================

void test_string_basic_operations() {
    TEST_SUITE("CacheObject String Basic Operations");

    // 测试创建 String 类型对象（默认构造函数创建 STRING 类型）
    CacheObject obj;
    EXPECT_EQ(obj.type(), ObjectType::STRING);

    // 测试设置和获取字符串
    obj.set_string("hello world");
    auto result = obj.get_string();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), std::string("hello world"));

    // 测试修改字符串
    obj.set_string("new value");
    result = obj.get_string();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), std::string("new value"));

    // 测试空字符串
    obj.set_string("");
    result = obj.get_string();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), std::string(""));

    // 测试长字符串
    std::string long_str(10000, 'x');
    obj.set_string(long_str);
    result = obj.get_string();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), long_str);

    std::cout << "✓ String basic operations test passed\n";
}

// ============================================================================
// CacheObject List 类型测试
// ============================================================================

void test_list_basic_operations() {
    TEST_SUITE("CacheObject List Basic Operations");

    CacheObject obj;

    // 测试 LPUSH
    obj.list_push("value1", true);  // left push
    obj.list_push("value2", true);
    obj.list_push("value3", true);
    EXPECT_EQ(obj.list_size(), size_t(3));
    EXPECT_EQ(obj.type(), ObjectType::LIST);

    // 测试 LRANGE
    auto range = obj.list_range(0, -1);
    EXPECT_EQ(range.size(), size_t(3));
    EXPECT_EQ(range[0], std::string("value3"));  // 最后 push 的在前面
    EXPECT_EQ(range[1], std::string("value2"));
    EXPECT_EQ(range[2], std::string("value1"));

    // 测试 RPUSH
    CacheObject obj2;
    obj2.list_push("a", false);  // right push
    obj2.list_push("b", false);
    obj2.list_push("c", false);
    auto range2 = obj2.list_range(0, -1);
    EXPECT_EQ(range2[0], std::string("a"));
    EXPECT_EQ(range2[1], std::string("b"));
    EXPECT_EQ(range2[2], std::string("c"));

    // 测试 LPOP
    auto popped = obj.list_pop(true);
    EXPECT_TRUE(popped.has_value());
    EXPECT_EQ(popped.value(), std::string("value3"));
    EXPECT_EQ(obj.list_size(), size_t(2));

    // 测试 RPOP
    popped = obj.list_pop(false);
    EXPECT_TRUE(popped.has_value());
    EXPECT_EQ(popped.value(), std::string("value1"));
    EXPECT_EQ(obj.list_size(), size_t(1));

    // 测试空列表 POP
    CacheObject empty_list;
    auto empty_pop = empty_list.list_pop(true);
    EXPECT_FALSE(empty_pop.has_value());

    std::cout << "✓ List basic operations test passed\n";
}

void test_list_range_operations() {
    TEST_SUITE("CacheObject List Range Operations");

    CacheObject obj;
    for (int i = 0; i < 10; ++i) {
        obj.list_push("item" + std::to_string(i), false);
    }

    // 测试正常范围
    auto range = obj.list_range(0, 4);
    EXPECT_EQ(range.size(), size_t(5));
    EXPECT_EQ(range[0], std::string("item0"));
    EXPECT_EQ(range[4], std::string("item4"));

    // 测试负索引
    range = obj.list_range(-3, -1);
    EXPECT_EQ(range.size(), size_t(3));
    EXPECT_EQ(range[0], std::string("item7"));
    EXPECT_EQ(range[2], std::string("item9"));

    // 测试超出范围
    range = obj.list_range(0, 100);
    EXPECT_EQ(range.size(), size_t(10));

    // 测试无效范围
    range = obj.list_range(5, 2);
    EXPECT_EQ(range.size(), size_t(0));

    std::cout << "✓ List range operations test passed\n";
}

// ============================================================================
// CacheObject Hash 类型测试
// ============================================================================

void test_hash_basic_operations() {
    TEST_SUITE("CacheObject Hash Basic Operations");

    CacheObject obj;

    // 测试 HSET
    obj.hash_set("field1", "value1");
    obj.hash_set("field2", "value2");
    obj.hash_set("field3", "value3");
    EXPECT_EQ(obj.hash_size(), size_t(3));
    EXPECT_EQ(obj.type(), ObjectType::HASH);

    // 测试 HGET
    auto value = obj.hash_get("field1");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), std::string("value1"));

    value = obj.hash_get("nonexistent");
    EXPECT_FALSE(value.has_value());

    // 测试 HEXISTS
    EXPECT_TRUE(obj.hash_exists("field1"));
    EXPECT_FALSE(obj.hash_exists("nonexistent"));

    // 测试 HDEL
    bool deleted = obj.hash_del("field2");
    EXPECT_TRUE(deleted);
    EXPECT_EQ(obj.hash_size(), size_t(2));
    EXPECT_FALSE(obj.hash_exists("field2"));

    deleted = obj.hash_del("nonexistent");
    EXPECT_FALSE(deleted);

    // 测试 HGETALL
    auto items = obj.hash_items();
    EXPECT_EQ(items.size(), size_t(2));

    // 测试 HKEYS
    auto fields = obj.hash_fields();
    EXPECT_EQ(fields.size(), size_t(2));

    std::cout << "✓ Hash basic operations test passed\n";
}

void test_hash_update_operations() {
    TEST_SUITE("CacheObject Hash Update Operations");

    CacheObject obj;

    // 测试覆盖已存在的字段
    obj.hash_set("field1", "value1");
    EXPECT_EQ(obj.hash_get("field1").value(), std::string("value1"));

    obj.hash_set("field1", "new_value");
    EXPECT_EQ(obj.hash_get("field1").value(), std::string("new_value"));
    EXPECT_EQ(obj.hash_size(), size_t(1));  // 长度不变

    std::cout << "✓ Hash update operations test passed\n";
}

// ============================================================================
// CacheObject Set 类型测试
// ============================================================================

void test_set_basic_operations() {
    TEST_SUITE("CacheObject Set Basic Operations");

    CacheObject obj;

    // 测试 SADD
    obj.set_add("member1");
    obj.set_add("member2");
    obj.set_add("member3");
    EXPECT_EQ(obj.set_size(), size_t(3));
    EXPECT_EQ(obj.type(), ObjectType::SET);

    // 测试重复添加
    obj.set_add("member1");
    EXPECT_EQ(obj.set_size(), size_t(3));  // 长度不变

    // 测试 SISMEMBER
    EXPECT_TRUE(obj.set_contains("member1"));
    EXPECT_FALSE(obj.set_contains("nonexistent"));

    // 测试 SREM
    bool removed = obj.set_remove("member2");
    EXPECT_TRUE(removed);
    EXPECT_EQ(obj.set_size(), size_t(2));
    EXPECT_FALSE(obj.set_contains("member2"));

    removed = obj.set_remove("nonexistent");
    EXPECT_FALSE(removed);

    // 测试 SMEMBERS
    auto members = obj.set_members();
    EXPECT_EQ(members.size(), size_t(2));

    std::cout << "✓ Set basic operations test passed\n";
}

// ============================================================================
// CacheObject ZSet 类型测试
// ============================================================================

void test_zset_basic_operations() {
    TEST_SUITE("CacheObject ZSet Basic Operations");

    CacheObject obj;

    // 测试 ZADD
    obj.zset_add("member1", 1.0);
    obj.zset_add("member2", 2.0);
    obj.zset_add("member3", 3.0);
    EXPECT_EQ(obj.zset_size(), size_t(3));
    EXPECT_EQ(obj.type(), ObjectType::ZSET);

    // 测试 ZSCORE
    auto score = obj.zset_score("member2");
    EXPECT_TRUE(score.has_value());
    EXPECT_EQ(score.value(), double(2.0));

    score = obj.zset_score("nonexistent");
    EXPECT_FALSE(score.has_value());

    // 测试更新分数
    obj.zset_add("member1", 5.0);
    EXPECT_EQ(obj.zset_size(), size_t(3));  // 长度不变
    EXPECT_EQ(obj.zset_score("member1").value(), double(5.0));

    // 测试 ZREM
    bool removed = obj.zset_remove("member2");
    EXPECT_TRUE(removed);
    EXPECT_EQ(obj.zset_size(), size_t(2));
    EXPECT_FALSE(obj.zset_score("member2").has_value());

    removed = obj.zset_remove("nonexistent");
    EXPECT_FALSE(removed);

    std::cout << "✓ ZSet basic operations test passed\n";
}

void test_zset_range_operations() {
    TEST_SUITE("CacheObject ZSet Range Operations");

    CacheObject obj;
    obj.zset_add("a", 1.0);
    obj.zset_add("b", 2.0);
    obj.zset_add("c", 3.0);
    obj.zset_add("d", 4.0);
    obj.zset_add("e", 5.0);

    // 测试 ZRANGE (按分数范围)
    auto range = obj.zset_range_by_score(2.0, 4.0);
    EXPECT_EQ(range.size(), size_t(3));
    EXPECT_EQ(range[0].first, std::string("b"));
    EXPECT_EQ(range[0].second, double(2.0));
    EXPECT_EQ(range[2].first, std::string("d"));
    EXPECT_EQ(range[2].second, double(4.0));

    // 测试边界
    range = obj.zset_range_by_score(1.0, 1.0);
    EXPECT_EQ(range.size(), size_t(1));
    EXPECT_EQ(range[0].first, std::string("a"));

    // 测试超出范围
    range = obj.zset_range_by_score(10.0, 20.0);
    EXPECT_EQ(range.size(), size_t(0));

    // 测试 ZALL
    auto all = obj.zset_all();
    EXPECT_EQ(all.size(), size_t(5));
    // 验证排序
    for (size_t i = 1; i < all.size(); ++i) {
        EXPECT_TRUE(all[i-1].second <= all[i].second);
    }

    std::cout << "✓ ZSet range operations test passed\n";
}

// ============================================================================
// 类型验证测试
// ============================================================================

void test_type_validation() {
    TEST_SUITE("CacheObject Type Validation");

    // 测试类型检查
    CacheObject str_obj;
    str_obj.set_string("test");
    EXPECT_EQ(str_obj.type(), ObjectType::STRING);

    CacheObject list_obj;
    list_obj.list_push("item", false);
    EXPECT_EQ(list_obj.type(), ObjectType::LIST);

    CacheObject hash_obj;
    hash_obj.hash_set("field", "value");
    EXPECT_EQ(hash_obj.type(), ObjectType::HASH);

    CacheObject set_obj;
    set_obj.set_add("member");
    EXPECT_EQ(set_obj.type(), ObjectType::SET);

    CacheObject zset_obj;
    zset_obj.zset_add("member", 1.0);
    EXPECT_EQ(zset_obj.type(), ObjectType::ZSET);

    std::cout << "✓ Type validation test passed\n";
}

// ============================================================================
// 内存大小测试
// ============================================================================

void test_memory_size() {
    TEST_SUITE("CacheObject Memory Size");

    // String
    CacheObject str_obj;
    str_obj.set_string("hello");
    size_t size = str_obj.memory_size();
    EXPECT_TRUE(size > 0);

    // List
    CacheObject list_obj;
    list_obj.list_push("item1", false);
    list_obj.list_push("item2", false);
    size = list_obj.memory_size();
    EXPECT_TRUE(size > 0);

    // Hash
    CacheObject hash_obj;
    hash_obj.hash_set("field1", "value1");
    size = hash_obj.memory_size();
    EXPECT_TRUE(size > 0);

    // Set
    CacheObject set_obj;
    set_obj.set_add("member1");
    size = set_obj.memory_size();
    EXPECT_TRUE(size > 0);

    // ZSet
    CacheObject zset_obj;
    zset_obj.zset_add("member1", 1.0);
    size = zset_obj.memory_size();
    EXPECT_TRUE(size > 0);

    std::cout << "✓ Memory size test passed\n";
}

// ============================================================================
// 主测试函数
// ============================================================================

void run_all_datatype_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running CacheObject DataType Tests\n";
    std::cout << "========================================\n\n";

    // String 测试
    test_string_basic_operations();

    // List 测试
    test_list_basic_operations();
    test_list_range_operations();

    // Hash 测试
    test_hash_basic_operations();
    test_hash_update_operations();

    // Set 测试
    test_set_basic_operations();

    // ZSet 测试
    test_zset_basic_operations();
    test_zset_range_operations();

    // 其他测试
    test_type_validation();
    test_memory_size();

    std::cout << "\n========================================\n";
    std::cout << "All CacheObject DataType Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
