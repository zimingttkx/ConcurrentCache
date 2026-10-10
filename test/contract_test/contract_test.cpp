// 契约测试：从协议外面看服务器有没有做它声称做的事。
//
// 这一支存在的理由是：#12 的门禁能抓"表脱钩"和"测试文件腐烂"，但抓不到
// "代码行为不对"。下面每条断言的是 Redis 的、也是本仓库 README/docs 承诺的
// 行为；其中多数今天会失败——失败才说明门禁有牙，修好一条就从
// ci/known-failures.txt 与标签上收紧一格。
#include "contract_test/resp_client.h"
#include "trace/test_assertions.h"
#include "command/string_cmd.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cc_server {
namespace testing {

namespace {

int contract_port() {
    // 由 pid 派生，避免和 runner 上并行的其它东西抢同一个端口。
    return 20000 + (static_cast<int>(getpid()) % 20000);
}

pid_t start_server(const std::string& binary, int port,
                   const std::string& config_path = "",
                   const std::string& log_path = "server.log") {
    pid_t pid = fork();
    if (pid != 0) return pid;

    int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    const std::string port_str = std::to_string(port);
    // config_path 为空时不带 --config，走"从 build 目录启动、用默认配置"的受支持用法
    char* const argv_default[] = {const_cast<char*>(binary.c_str()),
                                  const_cast<char*>("--port"),
                                  const_cast<char*>(port_str.c_str()),
                                  nullptr};
    char* const argv_with_config[] = {const_cast<char*>(binary.c_str()),
                                      const_cast<char*>("--port"),
                                      const_cast<char*>(port_str.c_str()),
                                      const_cast<char*>("--config"),
                                      const_cast<char*>(config_path.c_str()),
                                      nullptr};
    char* const* argv = config_path.empty() ? argv_default : argv_with_config;
    execv(binary.c_str(), const_cast<char**>(argv));
    _exit(127);  // 只有 execv 失败才会走到
}

bool wait_until_listening(int port, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        RespClient probe;
        if (probe.connect_to("127.0.0.1", port)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool do_cmd(RespClient& client, const std::vector<std::string>& args, Reply& out) {
    return client.send_command(args) && client.read_reply(out);
}

std::string reply_text(const Reply& reply) {
    if (reply.nil) return "<nil>";
    if (reply.type == ':') return "<int " + std::to_string(reply.integer) + ">";
    if (reply.type == '*') return "<array " + std::to_string(reply.elements.size()) + ">";
    return reply.str;
}

RespClient connected_client(int port) {
    RespClient client;
    client.connect_to("127.0.0.1", port);
    return client;
}

// 把服务器自己的输出尾巴打出来。start_server 把它的 stdout/stderr 重定向到
// ./server.log，红的时候这是唯一能分清"崩了 / 卡住了 / 只是行为不对"的证据。
void dump_server_log(int max_lines) {
    std::ifstream in("server.log");
    if (!in.good()) {
        std::cout << "[诊断] 读不到 server.log\n";
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > max_lines * 2) {
            lines.erase(lines.begin(), lines.begin() + max_lines);
        }
    }
    std::cout << "[诊断] server.log 末尾 " << max_lines << " 行：\n";
    const size_t start = lines.size() > static_cast<size_t>(max_lines)
                             ? lines.size() - static_cast<size_t>(max_lines) : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        std::cout << "  | " << lines[i] << "\n";
    }
}

}  // namespace

// 类型契约：非 string 键上的字符串命令必须回 WRONGTYPE，而不是回个空值
void run_type_contract_tests(int port) {
    TEST_SUITE("对象类型契约");

    RUN_TEST(get_on_hash_must_return_wrongtype) {
        RespClient client = connected_client(port);
        Reply reply;

        EXPECT_TRUE(do_cmd(client, {"SET", "hc_key", "hc_val"}, reply));
        EXPECT_TRUE(do_cmd(client, {"HSET", "hc_key", "f", "v"}, reply));
        EXPECT_TRUE(do_cmd(client, {"GET", "hc_key"}, reply));

        std::cout << "  GET on hash returned: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("WRONGTYPE"));
    });

    RUN_TEST(incr_on_hash_must_not_destroy_the_hash) {
        RespClient client = connected_client(port);
        Reply reply;

        EXPECT_TRUE(do_cmd(client, {"SET", "ic_key", "10"}, reply));
        EXPECT_TRUE(do_cmd(client, {"HSET", "ic_key", "f", "keepme"}, reply));

        // 今天的行为：incrby 读到对象里残留的 "10"，算出 11，然后整表被
        // insert_or_assign(CacheObject("11")) 覆盖掉。
        EXPECT_TRUE(do_cmd(client, {"INCR", "ic_key"}, reply));
        std::cout << "  INCR on hash returned: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("WRONGTYPE"));

        EXPECT_TRUE(do_cmd(client, {"HGET", "ic_key", "f"}, reply));
        std::cout << "  HGET after INCR returned: " << reply_text(reply) << "\n";
        EXPECT_TRUE(!reply.nil);
        EXPECT_EQ(reply.str, std::string("keepme"));
    });

    // INCRBY / DECRBY 在这之前只存在于复制写白名单里，CommandFactory 从来没注册
    // 它们 —— 客户端发过去得到的是 "unknown command"。
    RUN_TEST(incrby_and_decrby_behave_like_redis) {
        RespClient client = connected_client(port);
        Reply reply;

        EXPECT_TRUE(do_cmd(client, {"DEL", "ib_key"}, reply));
        // 键不存在时从 0 起算
        EXPECT_TRUE(do_cmd(client, {"INCRBY", "ib_key", "7"}, reply));
        EXPECT_EQ(reply.integer, 7LL);
        EXPECT_TRUE(do_cmd(client, {"INCRBY", "ib_key", "-2"}, reply));
        EXPECT_EQ(reply.integer, 5LL);
        EXPECT_TRUE(do_cmd(client, {"DECRBY", "ib_key", "3"}, reply));
        EXPECT_EQ(reply.integer, 2LL);

        // delta 不是整数 → 报错，且不能把键改成 0
        EXPECT_TRUE(do_cmd(client, {"SET", "ib_key", "42"}, reply));
        EXPECT_TRUE(do_cmd(client, {"INCRBY", "ib_key", "not_a_number"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"GET", "ib_key"}, reply));
        EXPECT_EQ(reply.str, std::string("42"));

        // 值是字符串但不是整数 → not an integer，而不是 WRONGTYPE
        EXPECT_TRUE(do_cmd(client, {"SET", "sb_key", "abc"}, reply));
        EXPECT_TRUE(do_cmd(client, {"INCRBY", "sb_key", "1"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(!reply.starts_with("WRONGTYPE"));

        // 哈希键 → WRONGTYPE，且哈希不能被毁掉
        EXPECT_TRUE(do_cmd(client, {"HSET", "hb_key", "f", "keepme"}, reply));
        EXPECT_TRUE(do_cmd(client, {"INCRBY", "hb_key", "1"}, reply));
        EXPECT_TRUE(reply.starts_with("WRONGTYPE"));
        EXPECT_TRUE(do_cmd(client, {"HGET", "hb_key", "f"}, reply));
        EXPECT_EQ(reply.str, std::string("keepme"));
    });
}

// TTL 契约：EXPIRE/PERSIST 必须真的改写那份唯一真相源
void run_ttl_contract_tests(int port) {
    TEST_SUITE("TTL 契约");

    RUN_TEST(persist_must_really_clear_the_expiry) {
        RespClient client = connected_client(port);
        Reply reply;

        EXPECT_TRUE(do_cmd(client, {"SETEX", "pc_key", "1", "survive"}, reply));
        EXPECT_EQ(reply.type, '+');

        EXPECT_TRUE(do_cmd(client, {"PERSIST", "pc_key"}, reply));
        std::cout << "  PERSIST returned: " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(1300));

        EXPECT_TRUE(do_cmd(client, {"GET", "pc_key"}, reply));
        std::cout << "  GET after PERSIST returned: " << reply_text(reply) << "\n";
        EXPECT_TRUE(!reply.nil);
        EXPECT_EQ(reply.str, std::string("survive"));
    });
}

// 原子性契约：容器写回不能丢更新（复合命令必须是分片锁内的原子操作）
void run_atomicity_contract_tests(int port) {
    TEST_SUITE("容器原子性契约");

    RUN_TEST(concurrent_lpush_loses_nothing) {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 50;
        constexpr int kExpected = kThreads * kPerThread;

        std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([t, port, &go]() {
                RespClient client = connected_client(port);
                Reply reply;
                while (!go.load()) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string value = "t" + std::to_string(t) + "_i" + std::to_string(i);
                    do_cmd(client, {"LPUSH", "atomic_list", value}, reply);
                }
            });
        }

        go.store(true);
        for (auto& worker : workers) {
            worker.join();
        }

        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"LLEN", "atomic_list"}, reply));
        std::cout << "  LLEN after " << kExpected << " LPUSH = " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, static_cast<long long>(kExpected));
    });

    // HSET / SADD 和 LPUSH 是同一个形状的读-改-写：get() 拿副本、改完 set() 整体
    // 覆盖，并发下后写的把先写的整个盖掉。列表那条测到了，这两条把它们各自钉住。
    RUN_TEST(concurrent_hash_and_set_writes_lose_nothing) {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 50;
        constexpr int kExpected = kThreads * kPerThread;

        auto hammer = [port](const char* cmd, const char* key) {
            std::atomic<bool> go{false};
            std::vector<std::thread> workers;
            workers.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                workers.emplace_back([t, port, cmd, key, &go]() {
                    RespClient client = connected_client(port);
                    Reply reply;
                    while (!go.load()) {
                        std::this_thread::yield();
                    }
                    for (int i = 0; i < kPerThread; ++i) {
                        const std::string field = "t" + std::to_string(t) + "_i" + std::to_string(i);
                        do_cmd(client, {cmd, key, field, field}, reply);
                    }
                });
            }
            go.store(true);
            for (auto& worker : workers) {
                worker.join();
            }
        };

        hammer("HSET", "atomic_hash");
        hammer("SADD", "atomic_set");

        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"HLEN", "atomic_hash"}, reply));
        std::cout << "  HLEN after " << kExpected << " HSET = " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, static_cast<long long>(kExpected));

        EXPECT_TRUE(do_cmd(client, {"SCARD", "atomic_set"}, reply));
        std::cout << "  SCARD after " << kExpected << " SADD = " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, static_cast<long long>(kExpected));
    });

    // 删空之后键必须消失：留一张空哈希/空集合，EXISTS 和 DBSIZE 都会多算
    RUN_TEST(emptying_a_container_deletes_the_key) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"HSET", "shrink_hash", "only", "v"}, reply));
        EXPECT_TRUE(do_cmd(client, {"HDEL", "shrink_hash", "only"}, reply));
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "shrink_hash"}, reply));
        std::cout << "  EXISTS after last HDEL = " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, 0LL);

        EXPECT_TRUE(do_cmd(client, {"LPUSH", "shrink_list", "a"}, reply));
        EXPECT_TRUE(do_cmd(client, {"LPOP", "shrink_list"}, reply));
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "shrink_list"}, reply));
        EXPECT_EQ(reply.integer, 0LL);
    });
}

// 协议健壮性：深度嵌套的数组不能把服务端的工作线程栈打穿。
// parse_one → parse_array → parse_one 是递归的，没有深度上限时，任何一个能连上
// 端口的客户端发 800KB 的 "*1\r\n" 重复串就够了 —— 不需要认证、不需要合法命令。
void run_protocol_limit_tests(int port) {
    TEST_SUITE("协议健壮性契约");

    RUN_TEST(deeply_nested_array_must_not_kill_the_server) {
        constexpr int kDepth = 200000;
        std::string payload;
        payload.reserve(static_cast<size_t>(kDepth) * 4 + 16);
        for (int i = 0; i < kDepth; ++i) {
            payload += "*1\r\n";
        }
        payload += "$2\r\nok\r\n";

        {
            RespClient client = connected_client(port);
            EXPECT_TRUE(client.send_raw(payload));
            Reply reply;
            // 回一个协议错误、或者直接断开连接都算"处理掉了"，这里不比内容。
            // 真正要证明的是进程还活着。
            client.read_reply(reply);
        }

        RespClient probe = connected_client(port);
        Reply reply;
        const bool answered = do_cmd(probe, {"PING"}, reply);
        std::cout << "  PING after 200000-level nesting: "
                  << (answered ? reply_text(reply) : "no reply") << "\n";
        EXPECT_TRUE(answered);
    });
}

// 容量契约：conf 里的 max_entries 必须真的决定键数量上限。
//
// 接线之前 main.cpp 从不读这个键，GlobalStorage 一直用类内的 200 万默认值，于是
// "配了上限却没有上限"：写 400 个键，一个都不会被淘汰。这条用例为这一点而存在——
// 它测的是配置到产品的连线，不是淘汰算法本身（算法在 storage 的单测里）。
void run_capacity_contract_tests(const std::string& binary) {
    TEST_SUITE("容量上限契约");

    constexpr int kMaxEntries = 60;
    constexpr int kWrites = 400;

    const int port = contract_port() + 1;  // 与主服务器错开一个端口
    const std::string tag = std::to_string(static_cast<long>(getpid()));
    const std::string conf_path = "/tmp/cc_maxentries_" + tag + ".conf";
    const std::string rdb_path = "/tmp/cc_maxentries_" + tag + ".rdb";
    const std::string log_path = "/tmp/cc_maxentries_" + tag + ".log";
    {
        std::ofstream out(conf_path, std::ios::trunc);
        out << "port = " << port << "\n"
            << "max_entries = " << kMaxEntries << "\n"
            << "rdb_path = " << rdb_path << "\n"
            << "rdb_save_interval = 0\n"
            << "log_level = 4\n"
            << "cluster_enabled = false\n";
    }

    pid_t server_pid = start_server(binary, port, conf_path, log_path);
    if (!wait_until_listening(port, 10000)) {
        std::cout << "带 max_entries 的服务器没有在 10s 内监听 " << port
                  << "（见 " << log_path << "）\n";
        EXPECT_TRUE(false);
        kill(server_pid, SIGTERM);
        waitpid(server_pid, nullptr, 0);
        std::remove(conf_path.c_str());
        return;
    }

    RUN_TEST(conf_max_entries_actually_limits_key_count) {
        RespClient client = connected_client(port);
        Reply reply;
        for (int i = 0; i < kWrites; ++i) {
            EXPECT_TRUE(do_cmd(client, {"SET", "mk_" + std::to_string(i), "v"}, reply));
        }
        EXPECT_TRUE(do_cmd(client, {"DBSIZE"}, reply));
        std::cout << "  写完 " << kWrites << " 个键后 DBSIZE = " << reply_text(reply) << "\n";
        // 淘汰在 90% 触发、回到 60%：留点余量，但必须远低于写入总数
        EXPECT_TRUE(reply.integer <= kMaxEntries);
        EXPECT_TRUE(reply.integer < kWrites / 2);
    });

    RUN_TEST(eviction_keeps_the_newest_and_drops_the_oldest) {
        RespClient client = connected_client(port);
        Reply reply;

        EXPECT_TRUE(do_cmd(client, {"GET", "mk_0"}, reply));
        std::cout << "  GET mk_0（最早写入） = " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.nil);

        EXPECT_TRUE(do_cmd(client, {"GET", "mk_" + std::to_string(kWrites - 1)}, reply));
        std::cout << "  GET mk_" << (kWrites - 1) << "（最新写入） = " << reply_text(reply) << "\n";
        EXPECT_TRUE(!reply.nil);
    });

    kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    std::remove(conf_path.c_str());
    std::remove(rdb_path.c_str());
    std::remove(log_path.c_str());
}

// ZADD 的分数入参契约：Redis 拒的必须拒，而且不许"半收"。
//
// 三段各管一件事：
//  - nan / inf：std::stod 认这些字面量。收进来之后 sorted set 的全序不再确定
//    （NaN 跟谁比都不成立），Redis 直接回 "value is NaN or Infinity"。
//  - 尾巴塞字符：std::stod("1.5abc") 返回 1.5 且不抛，于是客户端的拼写错误被
//    静默收成一个它并没有写的数；Redis 要求整个串都被吃掉。
//  - 被拒之后键不能留下：解析本来就在改库之前，这一条把它钉住，防止以后有人
//    把校验挪进 mutate 回调里。
//
// 最后一条顺便把 #80 的分数文本形态放进合并门禁：以前只有单元层直接调
// format_double，以及夜间档跟真 Redis 比；中间"真服务器 + 真 RESP"这一层是空的。
void run_zadd_score_contract_tests(int port) {
    TEST_SUITE("ZADD 分数入参契约");

    RUN_TEST(zadd_rejects_nan) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"ZADD", "zc_nan", "nan", "m"}, reply));
        std::cout << "  ZADD nan 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "zc_nan"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(zadd_rejects_infinity) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"ZADD", "zc_inf", "inf", "m"}, reply));
        std::cout << "  ZADD inf 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"ZADD", "zc_inf", "-infinity", "m"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "zc_inf"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(zadd_rejects_trailing_garbage) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"ZADD", "zc_garbage", "1.5abc", "m"}, reply));
        std::cout << "  ZADD 1.5abc 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "zc_garbage"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(zadd_rejects_out_of_range_without_creating_the_key) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"ZADD", "zc_huge", "1e400", "m"}, reply));
        std::cout << "  ZADD 1e400 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "zc_huge"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(zadd_accepts_legal_scores_and_echoes_them_like_redis) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client,
                           {"ZADD", "zc_fmt", "2.0", "a", "-0.5", "b", "1e-7", "c"},
                           reply));
        EXPECT_EQ(reply.integer, 3);

        EXPECT_TRUE(do_cmd(client, {"ZSCORE", "zc_fmt", "a"}, reply));
        std::cout << "  ZSCORE(a) 返回: " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.str, std::string("2"));

        EXPECT_TRUE(do_cmd(client, {"ZSCORE", "zc_fmt", "b"}, reply));
        EXPECT_EQ(reply.str, std::string("-0.5"));

        EXPECT_TRUE(do_cmd(client, {"ZSCORE", "zc_fmt", "c"}, reply));
        EXPECT_EQ(reply.str, std::string("0.0000001"));
    });
}

// EXPIRE 的非正秒数：Redis 的语义是"按立即过期删掉"，不是"失败"。
//
// 原来这一支回 :0 而且键原样留着。:0 在客户端眼里是"没设成功"，它不会重试，
// 于是 `EXPIRE k 0`（拿过期当删除的常见写法）留下一个永存的键。
// Redis（expireGenericCommand）的顺序是：先查键在不在（不在 → 0），再查目标
// 时间是否已经过去（已过 → 删键 + 1）。
void run_expire_semantics_contract_tests(int port) {
    TEST_SUITE("EXPIRE 语义契约");

    RUN_TEST(expire_zero_deletes_the_key_and_replies_one) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "ec_zero", "v"}, reply));
        EXPECT_TRUE(do_cmd(client, {"EXPIRE", "ec_zero", "0"}, reply));
        std::cout << "  EXPIRE 0 返回: " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, 1);
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "ec_zero"}, reply));
        EXPECT_EQ(reply.integer, 0);
        EXPECT_TRUE(do_cmd(client, {"TTL", "ec_zero"}, reply));
        EXPECT_EQ(reply.integer, -2);
    });

    RUN_TEST(expire_negative_deletes_the_key_and_replies_one) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "ec_neg", "v"}, reply));
        EXPECT_TRUE(do_cmd(client, {"EXPIRE", "ec_neg", "-5"}, reply));
        std::cout << "  EXPIRE -5 返回: " << reply_text(reply) << "\n";
        EXPECT_EQ(reply.integer, 1);
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "ec_neg"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    // 键不存在时仍然是 0，而且不许把键凭空建出来
    RUN_TEST(expire_on_missing_key_replies_zero_and_creates_nothing) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "ec_missing"}, reply));
        EXPECT_TRUE(do_cmd(client, {"EXPIRE", "ec_missing", "10"}, reply));
        EXPECT_EQ(reply.integer, 0);
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "ec_missing"}, reply));
        EXPECT_EQ(reply.integer, 0);
        // 同一套顺序用在非正秒数上：不存在的键 + EXPIRE 0 仍然是 0，不是 1
        EXPECT_TRUE(do_cmd(client, {"EXPIRE", "ec_missing", "0"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });
}

// SET 的选项契约。
//
// 这一条补的是最容易被真实客户端撞到、又完全没有覆盖的缺口：以前 SET 只认
// "SET key value" 三个参数，redis-py 的 set(..., ex=…) 和 Jedis 的 SetParams
// 直接打不进来。每个用例都同时断言"回复"和"落库的副作用"，只断言回复的话，
// 一个把 TTL 吞掉但照样回 +OK 的实现能蒙过去。
void run_set_option_contract_tests(int port) {
    TEST_SUITE("SET 选项契约");

    RUN_TEST(set_ex_sets_a_real_ttl) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_ex", "v", "EX", "100"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"TTL", "so_ex"}, reply));
        // 必须真的装了过期时间：0 或 -1 都说明 TTL 被吞了
        EXPECT_TRUE(reply.integer > 0 && reply.integer <= 100);
    });

    RUN_TEST(set_px_sets_millisecond_ttl) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_px", "v", "PX", "10000"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"PTTL", "so_px"}, reply));
        EXPECT_TRUE(reply.integer > 0 && reply.integer <= 10000);
    });

    RUN_TEST(set_rejects_non_positive_and_non_numeric_expiry) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_bad", "v", "EX", "0"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_bad", "v", "EX", "-5"}, reply));
        EXPECT_TRUE(reply.is_error());
        // "10abc" 不能读个前缀就当 10 收下
        EXPECT_TRUE(do_cmd(client, {"SET", "so_bad", "v", "EX", "10abc"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "so_bad"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(set_nx_only_writes_when_absent) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_nx", "first", "NX"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_nx", "second", "NX"}, reply));
        EXPECT_TRUE(reply.nil);                      // 条件不满足必须回 nil，不是 +OK
        EXPECT_TRUE(do_cmd(client, {"GET", "so_nx"}, reply));
        EXPECT_EQ(reply.str, std::string("first"));   // 而且旧值没被动过
    });

    RUN_TEST(set_xx_only_writes_when_present) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "so_xx"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_xx", "v", "XX"}, reply));
        EXPECT_TRUE(reply.nil);
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "so_xx"}, reply));
        EXPECT_EQ(reply.integer, 0);                  // 键不存在时 XX 不能把它创建出来
        EXPECT_TRUE(do_cmd(client, {"SET", "so_xx", "base"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_xx", "v2", "XX"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"GET", "so_xx"}, reply));
        EXPECT_EQ(reply.str, std::string("v2"));
    });

    RUN_TEST(set_get_returns_the_previous_value) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "so_get"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_get", "old", "GET"}, reply));
        EXPECT_TRUE(reply.nil);                        // 原来没有值 → nil
        EXPECT_TRUE(do_cmd(client, {"SET", "so_get", "new", "GET"}, reply));
        EXPECT_EQ(reply.str, std::string("old"));      // 回的是写入前的旧值，不是 +OK
        EXPECT_TRUE(do_cmd(client, {"GET", "so_get"}, reply));
        EXPECT_EQ(reply.str, std::string("new"));      // 新值确实写进去了
    });

    RUN_TEST(set_keepttl_preserves_expiry_but_plain_set_clears_it) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_keep", "v", "EX", "100"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_keep", "v2", "KEEPTTL"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"TTL", "so_keep"}, reply));
        EXPECT_TRUE(reply.integer > 0);                // KEEPTTL 保住了 TTL
        EXPECT_TRUE(do_cmd(client, {"SET", "so_keep", "v3"}, reply));
        EXPECT_TRUE(do_cmd(client, {"TTL", "so_keep"}, reply));
        EXPECT_EQ(reply.integer, -1);                  // 普通 SET 按 Redis 语义清掉 TTL
    });

    RUN_TEST(set_rejects_bad_option_combinations) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "NX", "XX"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "EX", "10", "PX", "10"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "EX", "10", "KEEPTTL"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "NX", "GET"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "FOO"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"SET", "so_combo", "v", "EX"}, reply));   // 选项缺参数
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "so_combo"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });

    RUN_TEST(set_options_are_case_insensitive) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_case", "v", "ex", "100", "nx"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"TTL", "so_case"}, reply));
        EXPECT_TRUE(reply.integer > 0 && reply.integer <= 100);
    });

    RUN_TEST(set_exat_in_the_past_deletes_instead_of_writing) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SET", "so_past", "base"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_past", "gone", "EXAT", "1000"}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "so_past"}, reply));
        EXPECT_EQ(reply.integer, 0);                   // 绝对时刻已过：键被删掉，不留新值
        EXPECT_TRUE(do_cmd(client, {"SET", "so_past2", "base"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_past2", "gone", "EXAT", "1000", "GET"}, reply));
        EXPECT_EQ(reply.str, std::string("base"));      // GET 变体回的是被删前的旧值
    });

    RUN_TEST(set_get_on_a_hash_key_is_wrongtype) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "so_type"}, reply));
        EXPECT_TRUE(do_cmd(client, {"HSET", "so_type", "f", "v"}, reply));
        EXPECT_TRUE(do_cmd(client, {"SET", "so_type", "v2", "GET"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_EQ(reply.str, std::string("WRONGTYPE Operation against a key holding the wrong kind of value"));
        EXPECT_TRUE(do_cmd(client, {"HGET", "so_type", "f"}, reply));
        EXPECT_EQ(reply.str, std::string("v"));         // 报 WRONGTYPE 时不许动原对象
    });
}

// 外部副本握手（PSYNC / SYNC / REPLCONF）必须显式拒绝。
//
// 改前 `PSYNC ? -1` 回 "+FULLRESYNC <runid> <offset>" 然后一个键都不发：真 Redis
// 拿 REPLICAOF 接上来会"握手成功、零数据、状态 online"，运维以为有副本了。
// 拒绝至少不骗人。另外 REPLCONF ACK 原来会把客户端报来的数字直接写进本节点的
// master_repl_offset，而 failover 的新鲜度判据读的就是这个值 —— 那个写入路径
// 已整条移除（偏移量没有客户端可见的读取口，所以这里断言命令被拒，写入不可达
// 由代码层面保证）。
void run_replica_handshake_contract_tests(int port) {
    TEST_SUITE("外部副本握手拒绝");

    RUN_TEST(psync_is_refused_and_does_not_answer_fullresync) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"PSYNC", "?", "-1"}, reply));
        std::cout << "  PSYNC ? -1 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        // 最关键的一条：不许再出现握手成功的样子
        EXPECT_TRUE(reply.str.find("FULLRESYNC") == std::string::npos);
    });

    RUN_TEST(psync_with_a_runid_is_also_refused) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"PSYNC", "abc123", "0"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.str.find("CONTINUE") == std::string::npos);
    });

    RUN_TEST(sync_is_refused) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"SYNC"}, reply));
        std::cout << "  SYNC 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
    });

    RUN_TEST(replconf_refuses_all_three_subcommands) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"REPLCONF", "listening-port", "6380"}, reply));
        EXPECT_TRUE(reply.is_error());
        // 这一条改前会回 +OK 并把 999999999 写进主节点的复制偏移量
        EXPECT_TRUE(do_cmd(client, {"REPLCONF", "ACK", "999999999"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"REPLCONF", "GETACK", "0"}, reply));
        EXPECT_TRUE(reply.is_error());
    });
}

// RESTORE 载荷的客户端可见契约：坏载荷必须报错，不能"回 +OK 但内容不对"。
//
// 改动前 RESTORE 用 std::getline 逐条读，遇到截断载荷会 break 出已读到的部分然后
// 照样回 +OK；而空/无标签的载荷会被当成 STRING 建出一个键。所以这三条钉的是
// "失败要看起来像失败"，配合 object_test.cpp 里的往返用例（那边钉框架本身）。
void run_restore_payload_contract_tests(int port) {
    TEST_SUITE("RESTORE 载荷契约");

    RUN_TEST(restore_accepts_a_well_formed_payload) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "rp_ok"}, reply));
        // STRING 载荷：类型标签一行 + 一条 "字节数\n原始字节" 记录
        EXPECT_TRUE(do_cmd(client, {"RESTORE", "rp_ok", "0", std::string("STRING\n3\nabc")}, reply));
        EXPECT_TRUE(reply.starts_with("OK"));
        EXPECT_TRUE(do_cmd(client, {"GET", "rp_ok"}, reply));
        EXPECT_EQ(reply.str, std::string("abc"));
    });

    RUN_TEST(restore_rejects_garbage_instead_of_creating_a_key) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "rp_junk"}, reply));
        EXPECT_TRUE(do_cmd(client, {"RESTORE", "rp_junk", "0", "total nonsense"}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "rp_junk"}, reply));
        EXPECT_EQ(reply.integer, 0);   // 改动前这里会建出一个空串键并回 +OK
    });

    RUN_TEST(restore_rejects_truncated_payload_without_leaving_a_partial_key) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"DEL", "rp_cut"}, reply));
        // 声明 3 个字节只给了 2 个
        EXPECT_TRUE(do_cmd(client, {"RESTORE", "rp_cut", "0", std::string("STRING\n3\nab")}, reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "rp_cut"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });
}

// CLUSTER MIGRATE 的 timeout 参数边界。
//
// #92 起这个值真的决定"阻塞多久"：命令跑在 SubReactor 的事件循环线程上，等目标回复
// 期间这条 reactor 上的所有连接都停着。改之前 timeout 被完全忽略，所以收下
// 999999999 无害；改之后它就是"把整条 reactor 冻十几天"。这里同时钉住校验顺序：
// 参数问题必须先于"集群没启用"报出来，否则单机模式下这条判据永远走不到。
void run_migrate_timeout_contract_tests(int port) {
    TEST_SUITE("CLUSTER MIGRATE 超时边界");

    RUN_TEST(migrate_rejects_timeout_above_the_ceiling) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client,
                           {"CLUSTER", "MIGRATE", "127.0.0.1", "16399", "mt_key", "999999999"},
                           reply));
        std::cout << "  超大 timeout 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("ERR timeout must be between"));
    });

    RUN_TEST(migrate_rejects_non_positive_timeout) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"CLUSTER", "MIGRATE", "127.0.0.1", "16399", "mt_key", "0"},
                           reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("ERR timeout must be between"));
        EXPECT_TRUE(do_cmd(client, {"CLUSTER", "MIGRATE", "127.0.0.1", "16399", "mt_key", "-5"},
                           reply));
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("ERR timeout must be between"));
    });

    RUN_TEST(migrate_rejects_a_timeout_with_trailing_garbage) {
        // std::stoi("5000abc") 返回 5000 且不抛 —— 对决定阻塞多久的参数不能这么猜
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client,
                           {"CLUSTER", "MIGRATE", "127.0.0.1", "16399", "mt_key", "5000abc"},
                           reply));
        std::cout << "  带尾巴的 timeout 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("ERR timeout is not an integer"));
    });

    RUN_TEST(valid_timeout_still_reports_cluster_disabled) {
        // 顺序探针：参数合法时才会走到"集群没启用"。改之前任何 timeout 都直接走到这里，
        // 所以这条同时证明上面三条是真的被边界拦下，而不是被 disabled 抢先。
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"CLUSTER", "MIGRATE", "127.0.0.1", "16399", "mt_key", "5000"},
                           reply));
        std::cout << "  合法 timeout 返回: " << reply_text(reply) << "\n";
        EXPECT_TRUE(reply.is_error());
        EXPECT_TRUE(reply.starts_with("ERR cluster mode is not enabled"));
        EXPECT_TRUE(do_cmd(client, {"EXISTS", "mt_key"}, reply));
        EXPECT_EQ(reply.integer, 0);
    });
}

// INFO memory：真实存在的常驻内存，而不是抄来的字段。
//
// 为什么单独有这一段：项目的三层内存池还没接进分配路径，所以没有可信的
// "分配器已用字节"可报。宁可让字段缺失（对比脚本会看到 Redis 有、本服务器没有），
// 也不要一个看着像真的的假数字。这里断言的是"报出来的必须是真的那一套"。
void run_info_memory_contract_tests(int port) {
    TEST_SUITE("INFO memory 段");

    RUN_TEST(info_memory_reports_real_rss_fields) {
        RespClient client = connected_client(port);
        Reply reply;
        EXPECT_TRUE(do_cmd(client, {"INFO", "memory"}, reply));
        EXPECT_TRUE(!reply.is_error());
        const std::string text = reply_text(reply);
        std::cout << "  INFO memory 前 120 字节: " << text.substr(0, 120) << std::endl;
        EXPECT_TRUE(text.find("# Memory") != std::string::npos);
        EXPECT_TRUE(text.find("used_memory_rss:") != std::string::npos);
        EXPECT_TRUE(text.find("maxmemory_policy:aru-random-shard-sampling") != std::string::npos);
        EXPECT_TRUE(text.find("used_memory:") == std::string::npos);  // 故意不报

        // VmRSS 必须是十进制数字，且非负；Linux 上一个活进程不可能为 0
        const std::size_t at = text.find("used_memory_rss:");
        EXPECT_TRUE(at != std::string::npos);
        if (at != std::string::npos) {
            std::size_t i = at + sizeof("used_memory_rss:") - 1;
            std::size_t digits = 0;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; ++digits; }
            EXPECT_TRUE(digits > 0);
#ifdef __linux__
            const unsigned long long rss = std::strtoull(text.c_str() + at + sizeof("used_memory_rss:") - 1, nullptr, 10);
            EXPECT_TRUE(rss > 0);
#endif
        }
    });

    RUN_TEST(rss_status_line_parser) {
        // 纯函数判据：换单位、没有数字、不是 VmRSS 行的三种形状
        EXPECT_EQ(cc_server::rss_bytes_from_status_line("VmRSS:	1024 kB"),
                  static_cast<uint64_t>(1024ull * 1024ull));
        EXPECT_EQ(cc_server::rss_bytes_from_status_line("VmRSS:	   0 kB"),
                  static_cast<uint64_t>(0));
        EXPECT_EQ(cc_server::rss_bytes_from_status_line("VmRSS:	kb"),
                  static_cast<uint64_t>(0));
        EXPECT_EQ(cc_server::rss_bytes_from_status_line("VmSize:	9999 kB"),
                  static_cast<uint64_t>(0));
    });

    RUN_TEST(info_all_contains_memory_section_and_unknown_section_still_errors) {
        RespClient client = connected_client(port);
        Reply all;
        EXPECT_TRUE(do_cmd(client, {"INFO", "all"}, all));
        EXPECT_TRUE(reply_text(all).find("# Memory") != std::string::npos);
        Reply bad;
        EXPECT_TRUE(do_cmd(client, {"INFO", "Memory"}, bad));   // 段名区分大小写
        EXPECT_TRUE(bad.is_error());
    });
}

void run_all_contract_tests() {
    const char* server_bin = std::getenv("CC_SERVER_BIN");
    // 环境变量没传来说明 CMake 接线断了，那必须是失败而不是跳过——
    // 否则这一支会在没人注意时永远"通过"。
    EXPECT_TRUE(server_bin != nullptr && server_bin[0] != '\0');
    if (server_bin == nullptr || server_bin[0] == '\0') return;

    const std::string binary = server_bin;
    if (access(binary.c_str(), X_OK) != 0) {
        std::cout << "找不到可执行的服务器：" << binary << "\n";
        EXPECT_TRUE(false);
        return;
    }

    const int port = contract_port();
    pid_t server_pid = start_server(binary, port);

    if (!wait_until_listening(port, 10000)) {
        std::cout << "服务器在 10s 内没有开始监听 127.0.0.1:" << port << "（见 server.log）\n";
        EXPECT_TRUE(false);
        if (server_pid > 0) {
            kill(server_pid, SIGTERM);
            waitpid(server_pid, nullptr, 0);
        }
        return;
    }

    run_type_contract_tests(port);
    run_ttl_contract_tests(port);
    run_expire_semantics_contract_tests(port);
    run_atomicity_contract_tests(port);
    run_protocol_limit_tests(port);
    run_zadd_score_contract_tests(port);
    run_set_option_contract_tests(port);
    run_replica_handshake_contract_tests(port);
    run_restore_payload_contract_tests(port);
    run_migrate_timeout_contract_tests(port);
    run_info_memory_contract_tests(port);
    run_capacity_contract_tests(binary);

    // 红了要能就地解释。WNOHANG 先问一次：服务器是自己死了还是还活着，决定了
    // 后面这些超时是崩溃还是行为不符 —— #29 那次 main 变红时就分不出来。
    int server_status = 0;
    const pid_t gone = waitpid(server_pid, &server_status, WNOHANG);
    const bool any_failed = g_test_stats().failed_tests.load() > 0;

    if (gone == server_pid) {
        std::cout << "\n[诊断] 服务器进程已提前退出："
                  << (WIFSIGNALED(server_status)
                          ? ("信号 " + std::to_string(WTERMSIG(server_status)))
                          : ("退出码 " + std::to_string(WEXITSTATUS(server_status))))
                  << "\n";
    } else if (any_failed) {
        std::cout << "\n[诊断] 服务器进程仍在运行 —— 客户端拿不到回复不等于它崩了\n";
    }

    if (gone == server_pid || any_failed) {
        dump_server_log(60);
    }

    if (gone != server_pid) {
        kill(server_pid, SIGTERM);
        waitpid(server_pid, nullptr, 0);
    }
}

}  // namespace testing
}  // namespace cc_server

int main() {
    cc_server::testing::run_all_contract_tests();
    return cc_server::testing::g_test_stats().failed_tests > 0 ? 1 : 0;
}
