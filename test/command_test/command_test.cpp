#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "../trace/test_assertions.h"
#include "command/command_factory.h"
#include "cache/storage.h"

namespace cc_server {
namespace testing {

// ============================================================================
// String 命令测试
// ============================================================================

void test_string_commands() {
    TEST_SUITE("String Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 SET
    auto set_cmd = factory.create_command("SET");
    EXPECT_TRUE(set_cmd != nullptr);
    std::string result = set_cmd->execute({"SET", "key1", "value1"});
    EXPECT_EQ(result, "+OK\r\n");

    // 测试 GET
    auto get_cmd = factory.create_command("GET");
    EXPECT_TRUE(get_cmd != nullptr);
    result = get_cmd->execute({"GET", "key1"});
    EXPECT_EQ(result, "$6\r\nvalue1\r\n");

    // 测试 GET 不存在的键
    result = get_cmd->execute({"GET", "nonexistent"});
    EXPECT_EQ(result, "$-1\r\n");

    // 测试 EXISTS
    auto exists_cmd = factory.create_command("EXISTS");
    EXPECT_TRUE(exists_cmd != nullptr);
    result = exists_cmd->execute({"EXISTS", "key1"});
    EXPECT_EQ(result, ":1\r\n");

    result = exists_cmd->execute({"EXISTS", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 DEL
    auto del_cmd = factory.create_command("DEL");
    EXPECT_TRUE(del_cmd != nullptr);
    result = del_cmd->execute({"DEL", "key1"});
    EXPECT_EQ(result, ":1\r\n");

    result = del_cmd->execute({"DEL", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    std::cout << "✓ String commands test passed\n";
}

// ============================================================================
// List 命令测试
// ============================================================================

void test_list_commands() {
    TEST_SUITE("List Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 LPUSH
    auto lpush_cmd = factory.create_command("LPUSH");
    EXPECT_TRUE(lpush_cmd != nullptr);
    std::string result = lpush_cmd->execute({"LPUSH", "list1", "value1"});
    EXPECT_EQ(result, ":1\r\n");

    result = lpush_cmd->execute({"LPUSH", "list1", "value2"});
    EXPECT_EQ(result, ":2\r\n");

    // 测试 RPUSH
    auto rpush_cmd = factory.create_command("RPUSH");
    EXPECT_TRUE(rpush_cmd != nullptr);
    result = rpush_cmd->execute({"RPUSH", "list2", "a"});
    EXPECT_EQ(result, ":1\r\n");

    result = rpush_cmd->execute({"RPUSH", "list2", "b"});
    EXPECT_EQ(result, ":2\r\n");

    // 测试 LLEN
    auto llen_cmd = factory.create_command("LLEN");
    EXPECT_TRUE(llen_cmd != nullptr);
    result = llen_cmd->execute({"LLEN", "list1"});
    EXPECT_EQ(result, ":2\r\n");

    // 测试 LPOP
    auto lpop_cmd = factory.create_command("LPOP");
    EXPECT_TRUE(lpop_cmd != nullptr);
    result = lpop_cmd->execute({"LPOP", "list1"});
    EXPECT_TRUE(result.find("$") == 0);  // 应该返回 bulk string

    // 测试 RPOP
    auto rpop_cmd = factory.create_command("RPOP");
    EXPECT_TRUE(rpop_cmd != nullptr);
    result = rpop_cmd->execute({"RPOP", "list2"});
    EXPECT_TRUE(result.find("$") == 0);

    // 测试 LRANGE
    lpush_cmd->execute({"LPUSH", "list3", "c"});
    lpush_cmd->execute({"LPUSH", "list3", "b"});
    lpush_cmd->execute({"LPUSH", "list3", "a"});

    auto lrange_cmd = factory.create_command("LRANGE");
    EXPECT_TRUE(lrange_cmd != nullptr);
    result = lrange_cmd->execute({"LRANGE", "list3", "0", "-1"});
    EXPECT_TRUE(result.find("*3\r\n") == 0);  // 应该返回 3 个元素

    std::cout << "✓ List commands test passed\n";
}

// ============================================================================
// Hash 命令测试
// ============================================================================

void test_hash_commands() {
    TEST_SUITE("Hash Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 HSET
    auto hset_cmd = factory.create_command("HSET");
    EXPECT_TRUE(hset_cmd != nullptr);
    std::string result = hset_cmd->execute({"HSET", "hash1", "field1", "value1"});
    EXPECT_EQ(result, ":1\r\n");

    result = hset_cmd->execute({"HSET", "hash1", "field2", "value2"});
    EXPECT_EQ(result, ":1\r\n");

    // 测试 HGET
    auto hget_cmd = factory.create_command("HGET");
    EXPECT_TRUE(hget_cmd != nullptr);
    result = hget_cmd->execute({"HGET", "hash1", "field1"});
    EXPECT_EQ(result, "$6\r\nvalue1\r\n");

    result = hget_cmd->execute({"HGET", "hash1", "nonexistent"});
    EXPECT_EQ(result, "$-1\r\n");

    // 测试 HEXISTS
    auto hexists_cmd = factory.create_command("HEXISTS");
    EXPECT_TRUE(hexists_cmd != nullptr);
    result = hexists_cmd->execute({"HEXISTS", "hash1", "field1"});
    EXPECT_EQ(result, ":1\r\n");

    result = hexists_cmd->execute({"HEXISTS", "hash1", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 HDEL
    auto hdel_cmd = factory.create_command("HDEL");
    EXPECT_TRUE(hdel_cmd != nullptr);
    result = hdel_cmd->execute({"HDEL", "hash1", "field1"});
    EXPECT_EQ(result, ":1\r\n");

    result = hdel_cmd->execute({"HDEL", "hash1", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 HLEN
    auto hlen_cmd = factory.create_command("HLEN");
    EXPECT_TRUE(hlen_cmd != nullptr);
    result = hlen_cmd->execute({"HLEN", "hash1"});
    EXPECT_EQ(result, ":1\r\n");

    // 测试 HGETALL
    hset_cmd->execute({"HSET", "hash2", "f1", "v1"});
    hset_cmd->execute({"HSET", "hash2", "f2", "v2"});

    auto hgetall_cmd = factory.create_command("HGETALL");
    EXPECT_TRUE(hgetall_cmd != nullptr);
    result = hgetall_cmd->execute({"HGETALL", "hash2"});
    EXPECT_TRUE(result.find("*4\r\n") == 0);  // 2 个字段 × 2 = 4 个元素

    std::cout << "✓ Hash commands test passed\n";
}

// ============================================================================
// Set 命令测试
// ============================================================================

void test_set_commands() {
    TEST_SUITE("Set Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 SADD
    auto sadd_cmd = factory.create_command("SADD");
    EXPECT_TRUE(sadd_cmd != nullptr);
    std::string result = sadd_cmd->execute({"SADD", "set1", "member1"});
    EXPECT_EQ(result, ":1\r\n");

    result = sadd_cmd->execute({"SADD", "set1", "member2"});
    EXPECT_EQ(result, ":1\r\n");

    // 重复添加
    result = sadd_cmd->execute({"SADD", "set1", "member1"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 SISMEMBER
    auto sismember_cmd = factory.create_command("SISMEMBER");
    EXPECT_TRUE(sismember_cmd != nullptr);
    result = sismember_cmd->execute({"SISMEMBER", "set1", "member1"});
    EXPECT_EQ(result, ":1\r\n");

    result = sismember_cmd->execute({"SISMEMBER", "set1", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 SCARD
    auto scard_cmd = factory.create_command("SCARD");
    EXPECT_TRUE(scard_cmd != nullptr);
    result = scard_cmd->execute({"SCARD", "set1"});
    EXPECT_EQ(result, ":2\r\n");

    // 测试 SREM
    auto srem_cmd = factory.create_command("SREM");
    EXPECT_TRUE(srem_cmd != nullptr);
    result = srem_cmd->execute({"SREM", "set1", "member1"});
    EXPECT_EQ(result, ":1\r\n");

    result = srem_cmd->execute({"SREM", "set1", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 SMEMBERS
    sadd_cmd->execute({"SADD", "set2", "a"});
    sadd_cmd->execute({"SADD", "set2", "b"});
    sadd_cmd->execute({"SADD", "set2", "c"});

    auto smembers_cmd = factory.create_command("SMEMBERS");
    EXPECT_TRUE(smembers_cmd != nullptr);
    result = smembers_cmd->execute({"SMEMBERS", "set2"});
    EXPECT_TRUE(result.find("*3\r\n") == 0);

    std::cout << "✓ Set commands test passed\n";
}

// ============================================================================
// ZSet 命令测试
// ============================================================================

void test_zset_commands() {
    TEST_SUITE("ZSet Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 ZADD
    auto zadd_cmd = factory.create_command("ZADD");
    EXPECT_TRUE(zadd_cmd != nullptr);
    std::string result = zadd_cmd->execute({"ZADD", "zset1", "1.0", "member1"});
    EXPECT_EQ(result, ":1\r\n");

    result = zadd_cmd->execute({"ZADD", "zset1", "2.0", "member2"});
    EXPECT_EQ(result, ":1\r\n");

    // 测试 ZSCORE
    auto zscore_cmd = factory.create_command("ZSCORE");
    EXPECT_TRUE(zscore_cmd != nullptr);
    result = zscore_cmd->execute({"ZSCORE", "zset1", "member1"});
    EXPECT_TRUE(result.find("$") == 0);  // 应该返回 bulk string

    result = zscore_cmd->execute({"ZSCORE", "zset1", "nonexistent"});
    EXPECT_EQ(result, "$-1\r\n");

    // 测试 ZCARD
    auto zcard_cmd = factory.create_command("ZCARD");
    EXPECT_TRUE(zcard_cmd != nullptr);
    result = zcard_cmd->execute({"ZCARD", "zset1"});
    EXPECT_EQ(result, ":2\r\n");

    // 测试 ZREM
    auto zrem_cmd = factory.create_command("ZREM");
    EXPECT_TRUE(zrem_cmd != nullptr);
    result = zrem_cmd->execute({"ZREM", "zset1", "member1"});
    EXPECT_EQ(result, ":1\r\n");

    result = zrem_cmd->execute({"ZREM", "zset1", "nonexistent"});
    EXPECT_EQ(result, ":0\r\n");

    // 测试 ZRANGE
    zadd_cmd->execute({"ZADD", "zset2", "1.0", "a"});
    zadd_cmd->execute({"ZADD", "zset2", "2.0", "b"});
    zadd_cmd->execute({"ZADD", "zset2", "3.0", "c"});

    auto zrange_cmd = factory.create_command("ZRANGE");
    EXPECT_TRUE(zrange_cmd != nullptr);
    result = zrange_cmd->execute({"ZRANGE", "zset2", "1.0", "2.0"});
    EXPECT_TRUE(result.find("*") == 0);  // 应该返回数组

    std::cout << "✓ ZSet commands test passed\n";
}

// ============================================================================
// TTL 命令测试
// ============================================================================

void test_ttl_commands() {
    TEST_SUITE("TTL Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 设置一个键
    auto set_cmd = factory.create_command("SET");
    set_cmd->execute({"SET", "key1", "value1"});

    // 测试 EXPIRE
    auto expire_cmd = factory.create_command("EXPIRE");
    EXPECT_TRUE(expire_cmd != nullptr);
    std::string result = expire_cmd->execute({"EXPIRE", "key1", "10"});
    EXPECT_EQ(result, ":1\r\n");

    // 测试 TTL
    auto ttl_cmd = factory.create_command("TTL");
    EXPECT_TRUE(ttl_cmd != nullptr);
    result = ttl_cmd->execute({"TTL", "key1"});
    EXPECT_TRUE(result.find(":") == 0);  // 应该返回整数

    // 测试 PERSIST
    auto persist_cmd = factory.create_command("PERSIST");
    EXPECT_TRUE(persist_cmd != nullptr);
    result = persist_cmd->execute({"PERSIST", "key1"});
    EXPECT_EQ(result, ":1\r\n");

    // 测试 SETEX
    auto setex_cmd = factory.create_command("SETEX");
    EXPECT_TRUE(setex_cmd != nullptr);
    result = setex_cmd->execute({"SETEX", "key2", "10", "value2"});
    EXPECT_EQ(result, "+OK\r\n");

    std::cout << "✓ TTL commands test passed\n";
}

// ============================================================================
// 通用命令测试
// ============================================================================

void test_general_commands() {
    TEST_SUITE("General Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试 PING
    auto ping_cmd = factory.create_command("PING");
    EXPECT_TRUE(ping_cmd != nullptr);
    std::string result = ping_cmd->execute({"PING"});
    EXPECT_EQ(result, "+PONG\r\n");

    // 添加一些数据
    auto set_cmd = factory.create_command("SET");
    set_cmd->execute({"SET", "key1", "value1"});
    set_cmd->execute({"SET", "key2", "value2"});
    set_cmd->execute({"SET", "key3", "value3"});

    // 测试 DBSIZE
    auto dbsize_cmd = factory.create_command("DBSIZE");
    EXPECT_TRUE(dbsize_cmd != nullptr);
    result = dbsize_cmd->execute({"DBSIZE"});
    EXPECT_EQ(result, ":3\r\n");

    // 测试 FLUSHDB
    auto flushdb_cmd = factory.create_command("FLUSHDB");
    EXPECT_TRUE(flushdb_cmd != nullptr);
    result = flushdb_cmd->execute({"FLUSHDB"});
    EXPECT_EQ(result, "+OK\r\n");

    result = dbsize_cmd->execute({"DBSIZE"});
    EXPECT_EQ(result, ":0\r\n");

    std::cout << "✓ General commands test passed\n";
}

// ============================================================================
// RDB 命令测试
// ============================================================================

void test_rdb_commands() {
    TEST_SUITE("RDB Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 添加一些数据
    auto set_cmd = factory.create_command("SET");
    set_cmd->execute({"SET", "key1", "value1"});
    set_cmd->execute({"SET", "key2", "value2"});

    // 测试 SAVE
    auto save_cmd = factory.create_command("SAVE");
    EXPECT_TRUE(save_cmd != nullptr);
    std::string result = save_cmd->execute({"SAVE"});
    EXPECT_EQ(result, "+OK\r\n");

    // 测试 BGSAVE
    auto bgsave_cmd = factory.create_command("BGSAVE");
    EXPECT_TRUE(bgsave_cmd != nullptr);
    result = bgsave_cmd->execute({"BGSAVE"});
    // BGSAVE 可能返回 OK 或者 "Background save already in progress"
    EXPECT_TRUE(result.find("+") == 0);

    // 测试 LASTSAVE
    auto lastsave_cmd = factory.create_command("LASTSAVE");
    EXPECT_TRUE(lastsave_cmd != nullptr);
    result = lastsave_cmd->execute({"LASTSAVE"});
    EXPECT_TRUE(result.find(":") == 0);  // 应该返回时间戳

    std::cout << "✓ RDB commands test passed\n";
}

// ============================================================================
// 无效命令测试
// ============================================================================

void test_invalid_commands() {
    TEST_SUITE("Invalid Commands");

    GlobalStorage storage;
    CommandFactory factory(storage);

    // 测试不存在的命令
    auto cmd = factory.create_command("NONEXISTENT");
    EXPECT_TRUE(cmd == nullptr);

    // 测试参数不足
    auto get_cmd = factory.create_command("GET");
    std::string result = get_cmd->execute({"GET"});  // 缺少 key 参数
    EXPECT_TRUE(result.find("-ERR") == 0);

    std::cout << "✓ Invalid commands test passed\n";
}

// ============================================================================
// 主测试函数
// ============================================================================

void run_all_command_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Command Layer Tests\n";
    std::cout << "========================================\n\n";

    test_string_commands();
    test_list_commands();
    test_hash_commands();
    test_set_commands();
    test_zset_commands();
    test_ttl_commands();
    test_general_commands();
    test_rdb_commands();
    test_invalid_commands();

    std::cout << "\n========================================\n";
    std::cout << "All Command Layer Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
