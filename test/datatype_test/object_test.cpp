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
// STRING 表示的类型契约测试
// ============================================================================

// get_string() 返回空 optional 是调用方区分"键里装的是别的类型"和
// "键里装的是空字符串"的唯一依据（GET 回 WRONGTYPE 还是回 ""、INCR 回
// WRONGTYPE 还是回 not an integer，全走这条路）。这里同时锁死两半：
// 读取侧要按 type_ 拦，写入侧容器接管 key 时要把旧字符串真的扔掉。
void test_string_type_contract() {
    TEST_SUITE("CacheObject String Type Contract");

    // 空字符串是合法的 STRING，必须与"不是字符串"区分开
    CacheObject empty_str(std::string(""));
    EXPECT_TRUE(empty_str.get_string().has_value());
    EXPECT_EQ(empty_str.get_string().value(), std::string(""));

    // Hash 接管
    CacheObject as_hash(std::string("10"));
    EXPECT_TRUE(as_hash.hash_set("f", "keepme"));
    EXPECT_EQ(as_hash.type(), ObjectType::HASH);
    EXPECT_TRUE(!as_hash.get_string().has_value());
    EXPECT_EQ(as_hash.hash_get("f").value(), std::string("keepme"));

    // Set 接管
    CacheObject as_set(std::string("10"));
    EXPECT_TRUE(as_set.set_add("m"));
    EXPECT_EQ(as_set.type(), ObjectType::SET);
    EXPECT_TRUE(!as_set.get_string().has_value());

    // ZSet 接管
    CacheObject as_zset(std::string("10"));
    EXPECT_TRUE(as_zset.zset_add("m", 1.0));
    EXPECT_EQ(as_zset.type(), ObjectType::ZSET);
    EXPECT_TRUE(!as_zset.get_string().has_value());

    // List 接管：旧值保留为首元素，但不再能被当作 string 读出来
    CacheObject as_list(std::string("10"));
    EXPECT_TRUE(as_list.list_push("20", true));
    EXPECT_EQ(as_list.type(), ObjectType::LIST);
    EXPECT_TRUE(!as_list.get_string().has_value());
    EXPECT_EQ(as_list.list_size(), static_cast<size_t>(2));

    // 换回 STRING 之后必须能正常读出来：判定跟着 type_ 走，不是一次性标记
    CacheObject back_to_str(std::string("x"));
    EXPECT_TRUE(back_to_str.hash_set("f", "v"));
    EXPECT_TRUE(!back_to_str.get_string().has_value());
    back_to_str.set_string("now-a-string");
    EXPECT_TRUE(back_to_str.get_string().has_value());

    std::cout << "✓ String type contract test passed\n";
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
// serialize / deserialize —— 迁移与复制键流用的载荷框架
//
// 旧写法每条记录以 "\n" 结尾，于是内容里带换行的元素会被提前收条：
// LPUSH k "a\nb" 之后迁移/复制出去，解码器按声明的数量读到 "a" 就停了，"b" 丢掉，
// 而且它照样回 +OK。下面四条钉的是"原样读回来"，不是"读出来像是对的"。
// ============================================================================
void test_serialize_roundtrip_with_newlines_in_members() {
    TEST_SUITE("CacheObject 载荷框架往返");

    CacheObject src;
    const std::vector<std::string> elems = {"a\nb", "plain", "\n", "", "c\r\nd", "尾随换行\n"};
    for (const auto& e : elems) {
        src.list_push(e);
    }

    const std::string payload = src.serialize();

    CacheObject dst;
    std::string err;
    EXPECT_TRUE(dst.deserialize(payload, err));
    if (dst.list_size() != elems.size()) {
        std::cout << "  元素数量不符: 读到 " << dst.list_size()
                  << " 个，声明 " << elems.size() << " 个\n";
    }
    EXPECT_EQ(dst.list_size(), elems.size());

    const std::vector<std::string> got = dst.list_range(0, -1);
    for (size_t i = 0; i < elems.size(); ++i) {
        EXPECT_TRUE(i < got.size());
        if (i < got.size()) {
            EXPECT_EQ(got[i], elems[i]);
        }
    }
}

void test_serialize_roundtrip_hash_set_zset() {
    TEST_SUITE("CacheObject 载荷框架往返");

    // 一个 CacheObject 只能装一种类型，所以三种载荷各用各的源对象
    CacheObject src_hash;
    src_hash.hash_set("f\n1", "v\n2");
    src_hash.hash_set("normal", "value");
    CacheObject src_set;
    src_set.set_add("m\n1");
    src_set.set_add("plain");
    CacheObject src_zset;
    src_zset.zset_add("z\nmember", 0.123456789);
    src_zset.zset_add("second", 2.0);

    CacheObject dst_hash, dst_set, dst_zset;
    std::string err;
    EXPECT_TRUE(dst_hash.deserialize(src_hash.serialize(), err));
    EXPECT_TRUE(dst_set.deserialize(src_set.serialize(), err));
    EXPECT_TRUE(dst_zset.deserialize(src_zset.serialize(), err));

    EXPECT_EQ(dst_hash.hash_size(), static_cast<size_t>(2));
    EXPECT_EQ(dst_hash.hash_get("f\n1").value_or("<没有读回来>"), std::string("v\n2"));
    EXPECT_EQ(dst_set.set_size(), static_cast<size_t>(2));
    EXPECT_TRUE(dst_set.set_contains("m\n1"));
    EXPECT_EQ(dst_zset.zset_size(), static_cast<size_t>(2));

    // 分数要按位原样回来：%.17g + from_chars 这一对是 P1-13 的约定，
    // 若读侧退回宽松解析，0.123456789 会先被截成 6 位小数
    bool precision_kept = false;
    for (const auto& item : dst_zset.zset_all()) {
        if (item.first == "z\nmember") {
            precision_kept = (item.second == 0.123456789);
        }
    }
    EXPECT_TRUE(precision_kept);
}

void test_serialize_roundtrip_string_with_newlines() {
    TEST_SUITE("CacheObject 载荷框架往返");

    const std::string value = "第一行\n第二行\n";
    const std::string payload = CacheObject(value).serialize();

    CacheObject dst;
    std::string err;
    EXPECT_TRUE(dst.deserialize(payload, err));
    EXPECT_EQ(dst.get_string().value_or("<没有读回来>"), value);

    // 记录读完还有尾巴 = 发的人用的是别的框架。必须拒，不能让两种字节流互相误读
    CacheObject bad;
    std::string bad_err;
    EXPECT_TRUE(!bad.deserialize(payload + "extra", bad_err));
}

void test_deserialize_fails_closed_on_malformed_payload() {
    TEST_SUITE("CacheObject 载荷框架 fail-closed");

    CacheObject src;
    src.list_push("a\nb");
    src.list_push("c");
    const std::string payload = src.serialize();

    // 截掉最后一个字节：整条必须失败，且不许留下半个键（旧实现会交出已读到的部分然后回成功）
    const std::string truncated = payload.substr(0, payload.size() - 1);
    CacheObject cut;
    std::string cut_err;
    EXPECT_TRUE(!cut.deserialize(truncated, cut_err));
    EXPECT_EQ(cut.list_size(), static_cast<size_t>(0));
    std::cout << "  截断载荷的拒绝理由: " << cut_err << "\n";

    // 旧框架（"LIST\n1\na\nb\n"，数量 1 后面其实跟着两条内容）必须被拒
    CacheObject legacy;
    std::string legacy_err;
    EXPECT_TRUE(!legacy.deserialize(std::string("LIST") + "\n1\na\nb\n", legacy_err));
    std::cout << "  旧框架载荷的拒绝理由: " << legacy_err << "\n";

    CacheObject junk;
    std::string junk_err;
    EXPECT_TRUE(!junk.deserialize("", junk_err));                          // 空载荷
    EXPECT_TRUE(!junk.deserialize("no-type-tag-here", junk_err));          // 没有换行
    EXPECT_TRUE(!junk.deserialize("STREAM\n3\nabc", junk_err));            // 类型不认识
    EXPECT_TRUE(!junk.deserialize("ZSET\n1\nmember\nnot-a-number\n", junk_err));  // 分数不是数
    EXPECT_EQ(junk.zset_size(), static_cast<size_t>(0));
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

    // 载荷框架（MIGRATE / 复制键流）往返与 fail-closed
    test_serialize_roundtrip_with_newlines_in_members();
    test_serialize_roundtrip_hash_set_zset();
    test_serialize_roundtrip_string_with_newlines();
    test_deserialize_fails_closed_on_malformed_payload();

    // 其他测试
    test_string_type_contract();
    test_type_validation();
    test_memory_size();

    std::cout << "\n========================================\n";
    std::cout << "All CacheObject DataType Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
