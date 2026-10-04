#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <zlib.h>
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

// 损坏的 RDB 必须在"动存储"之前就被拦下。
// 旧顺序是逐条写进 storage、读到文件末尾才计算 CRC —— 于是半个数据集已经落库，
// load() 才返回 false，而调用方分不清"文件坏了"和"根本没有文件"，服务器照常
// 拿着这批残缺数据对外服务。
void test_rdb_corrupt_file_leaves_storage_untouched() {
    TEST_SUITE("RDB Corrupt File");

    const std::string path = "/tmp/test_rdb_corrupt.rdb";

    GlobalStorage src;
    CacheObject a;
    a.set_string("AAA");
    CacheObject b;
    b.set_string("BBB");
    src.set("k1", a);
    src.set("k2", b);

    auto& rdb = RdbPersistence::instance();
    EXPECT_TRUE(rdb.save(path, src));

    // 翻掉正文中间的一个字节：magic、版本、CRC 尾都在，只有内容不符
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        EXPECT_TRUE(f.good());
        f.seekg(10);
        char c = 0;
        f.get(c);
        f.seekp(10);
        f.put(static_cast<char>(c ^ 0xFF));
    }

    GlobalStorage dst;
    EXPECT_TRUE(!rdb.load(path, dst));
    // 关键断言：一个键都不许进来
    EXPECT_EQ(dst.size(), static_cast<size_t>(0));
    EXPECT_TRUE(!dst.get("k1").has_value());

    std::remove(path.c_str());
}

// BGSAVE 的状态机：完成之后必须是 SUCCESS 且不再 in-progress。
// 旧代码把 "IN_PROGRESS" 的赋值写在起线程之后，小快照先跑完就会被这一行盖回
// IN_PROGRESS，INFO 里永远显示"还在保存"。
void test_rdb_bgsave_reports_success() {
    TEST_SUITE("RDB BGSAVE State Machine");

    GlobalStorage storage;
    CacheObject a;
    a.set_string("1");
    storage.set("bk1", a);
    storage.set("bk2", a);
    storage.set("bk3", a);

    auto& rdb = RdbPersistence::instance();
    // 前一个用例可能还留着一次没跑完的 BGSAVE，先排干再断言
    rdb.wait_for_bgsave(5000);

    const std::string path = "/tmp/test_rdb_bgsave_state.rdb";
    std::remove(path.c_str());

    EXPECT_TRUE(rdb.save_in_background(path, storage));
    EXPECT_TRUE(rdb.wait_for_bgsave(5000));
    EXPECT_TRUE(!rdb.is_bgsave_in_progress());
    EXPECT_TRUE(rdb.get_last_bgsave_status() == BgsaveStatus::SUCCESS);
    EXPECT_EQ(rdb.get_last_bgsave_keys(), static_cast<size_t>(3));

    // 快照必须真的落盘、且内容对得上
    GlobalStorage reloaded;
    EXPECT_TRUE(rdb.load(path, reloaded));
    EXPECT_EQ(reloaded.size(), static_cast<size_t>(3));

    std::remove(path.c_str());
}

void test_rdb_empty_file_is_a_valid_start_point() {
    TEST_SUITE("RDB Empty File");

    auto& rdb = RdbPersistence::instance();

    // 0 字节 = 还没有数据，不是损坏。e2e 脚本正是 touch 一个空 dump.rdb 来
    // 表示"从空存储启动"；把它判成损坏会让服务器根本起不来。
    const std::string empty_path = "/tmp/test_rdb_empty.rdb";
    {
        std::ofstream out(empty_path, std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(out.good());
    }

    GlobalStorage dst;
    EXPECT_TRUE(rdb.load(empty_path, dst));
    EXPECT_EQ(dst.size(), static_cast<size_t>(0));
    std::remove(empty_path.c_str());

    // 而"短到装不下头部"的文件仍然必须被拒绝，证明上一条不是把校验整体放宽了
    const std::string truncated_path = "/tmp/test_rdb_truncated.rdb";
    {
        std::ofstream out(truncated_path, std::ios::binary | std::ios::trunc);
        out.write("RED", 3);
    }

    GlobalStorage dst2;
    EXPECT_TRUE(!rdb.load(truncated_path, dst2));
    EXPECT_EQ(dst2.size(), static_cast<size_t>(0));
    std::remove(truncated_path.c_str());
}

// ============================================================================
// 手工构造 RDB 的小工具
//
// 下面几条要测的是"正文与头部对不上"的文件。这种文件 save() 写不出来，只能手拼。
// 尾部 4 字节按 save() 的算法补上 CRC32（覆盖 [0, 文件尾-4) 的全部字节），好让文件
// 先过 CRC 校验、真的走到正文解析——不然测到的是 CRC 那一关，跟这里想证明的东西无关。
// ============================================================================

namespace {

void put_u32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>(v & 0xFFu));
}

void put_u8(std::string& out, uint8_t v) {
    out.push_back(static_cast<char>(v));
}

std::string seal_rdb(const std::string& body) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()));
    std::string out = body;
    put_u32(out, static_cast<uint32_t>(crc));
    return out;
}

// 头部：MAGIC + VERSION + db 数量 + 第一个 db 的键值对数量
std::string rdb_header(uint32_t db_count, uint32_t kv_count) {
    std::string out;
    put_u32(out, kRdbMagic);
    out.append(reinterpret_cast<const char*>(kRdbVersion), 4);
    put_u32(out, db_count);
    put_u32(out, kv_count);
    return out;
}

void write_raw_file(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
}

}  // namespace

// 值类型字节不在 STRING/LIST/HASH/SET/ZSET 之内。
//
// 修之前：这条记录算"解析失败"，可 load() 里的循环没人看返回值就接着读下一条，
// 而未知类型后面的正文本该按那个类型来解析——游标就此错位。这里 kv_count 只有 1，
// 循环结束后那个 0xFF 正好被当成 EOF 标记读走，于是整个文件报成"加载成功"，
// 键一条都没进存储。损坏被报成成功，客户端无从分辨。
void test_rdb_unknown_value_type_is_not_a_successful_load() {
    TEST_SUITE("RDB Unknown Value Type");

    const std::string path = "/tmp/test_rdb_unknown_type.rdb";

    std::string body = rdb_header(1, 1);
    put_u8(body, 0x00);                                        // 非 TTL 标记
    put_u32(body, 1);
    body.append("k", 1);                                       // key = "k"
    put_u8(body, 0x07);                                        // 没有这个 value 类型
    put_u8(body, static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER));
    write_raw_file(path, seal_rdb(body));

    auto& rdb = RdbPersistence::instance();
    GlobalStorage dst;
    EXPECT_TRUE(!rdb.load(path, dst));
    EXPECT_EQ(dst.size(), static_cast<size_t>(0));

    std::remove(path.c_str());
}

// 头部声称 0 条记录，正文却在 EOF 标记后面还剩一个字节。
//
// 修之前：循环什么都不读，直接读到那个 0xFF，标记对得上就 return true，多出来的字节
// 没人管。EOF 标记原本只按 WARN 处理，正文长度与头部计数不一致这件事等于没检查。
void test_rdb_body_longer_than_header_claims_is_rejected() {
    TEST_SUITE("RDB Trailing Body Bytes");

    const std::string path = "/tmp/test_rdb_trailing_body.rdb";

    std::string body = rdb_header(1, 0);
    put_u8(body, static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER));
    put_u8(body, 0x00);                                        // 头部没算到的多余字节
    write_raw_file(path, seal_rdb(body));

    auto& rdb = RdbPersistence::instance();
    GlobalStorage dst;
    EXPECT_TRUE(!rdb.load(path, dst));

    std::remove(path.c_str());
}

// value 的长度字段声称 4 字节，正文里只剩 2 字节——多出来的 2 字节是尾部的 CRC。
//
// 这条最能说明边界检查为什么必须在分配之前做：fread 照样读得满（文件里确实还有
// 那些字节），所以修之前不会报错，而是把 CRC 的字节当成 value 的内容收下，
// 最后拿 CRC 的第三个字节去比 EOF 标记——比不上也只 WARN，load() 返回 true，
// 存储里多出一个键为 "ke"、值是半个校验和的键。长度字段没跟"正文还剩多少"比过。
void test_rdb_string_length_beyond_body_is_rejected() {
    TEST_SUITE("RDB String Length Beyond Body");

    const std::string path = "/tmp/test_rdb_length_borrows_crc.rdb";

    std::string body = rdb_header(1, 1);
    put_u8(body, 0x00);                                        // 非 TTL 标记
    put_u32(body, 2);
    body.append("ke", 2);                                      // key = "ke"
    put_u8(body, static_cast<uint8_t>(RdbValueType::STRING));
    put_u32(body, 4);                                          // value 声称 4 字节
    put_u8(body, 0xAA);                                        // 正文只剩这 1 字节
    put_u8(body, static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER));
    write_raw_file(path, seal_rdb(body));                      // seal 会再追加 4 字节 CRC

    auto& rdb = RdbPersistence::instance();
    GlobalStorage dst;
    EXPECT_TRUE(!rdb.load(path, dst));
    EXPECT_EQ(dst.size(), static_cast<size_t>(0));

    std::remove(path.c_str());
}

// 一个几十字节的 dump.rdb 把 key 长度写成 0xFFFFFFFF。
//
// 这一条是防回归的护栏，不是证据：修之前它会先 malloc 一个 4GB 的 std::string（构造
// 会把每个字节都写一遍，不是保留地址），申请失败抛 bad_alloc、申请成功则 fread 读不满
// 抛异常，两种结局都返回 false，新旧实现都能过这条。它守的是顺序——检查必须在分配之前，
// 否则启动时一个几十字节的 dump.rdb 就够把进程换掉。
void test_rdb_huge_string_length_is_refused_without_allocating() {
    TEST_SUITE("RDB Huge String Length");

    const std::string path = "/tmp/test_rdb_huge_length.rdb";

    std::string body = rdb_header(1, 1);
    put_u8(body, 0x00);
    put_u32(body, 0xFFFFFFFFu);                                // key 长度 = 4GB-1
    put_u8(body, static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER));
    write_raw_file(path, seal_rdb(body));

    auto& rdb = RdbPersistence::instance();
    GlobalStorage dst;
    EXPECT_TRUE(!rdb.load(path, dst));
    EXPECT_EQ(dst.size(), static_cast<size_t>(0));

    std::remove(path.c_str());
}

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
    test_rdb_corrupt_file_leaves_storage_untouched();
    test_rdb_empty_file_is_a_valid_start_point();
    test_rdb_unknown_value_type_is_not_a_successful_load();
    test_rdb_body_longer_than_header_claims_is_rejected();
    test_rdb_string_length_beyond_body_is_rejected();
    test_rdb_huge_string_length_is_refused_without_allocating();
    test_rdb_stats();
    test_rdb_bgsave();
    test_rdb_bgsave_reports_success();

    std::cout << "\n========================================\n";
    std::cout << "All RDB Persistence Tests Passed!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
