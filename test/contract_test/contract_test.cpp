// 契约测试：从协议外面看服务器有没有做它声称做的事。
//
// 这一支存在的理由是：#12 的门禁能抓"表脱钩"和"测试文件腐烂"，但抓不到
// "代码行为不对"。下面每条断言的是 Redis 的、也是本仓库 README/docs 承诺的
// 行为；其中多数今天会失败——失败才说明门禁有牙，修好一条就从
// ci/known-failures.txt 与标签上收紧一格。
#include "contract_test/resp_client.h"
#include "trace/test_assertions.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

pid_t start_server(const std::string& binary, int port) {
    pid_t pid = fork();
    if (pid != 0) return pid;

    const std::string log_path = "server.log";
    int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    const std::string port_str = std::to_string(port);
    char* const argv[] = {const_cast<char*>(binary.c_str()),
                          const_cast<char*>("--port"),
                          const_cast<char*>(port_str.c_str()),
                          nullptr};
    execv(binary.c_str(), argv);
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
    run_atomicity_contract_tests(port);
    run_protocol_limit_tests(port);

    kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
}

}  // namespace testing
}  // namespace cc_server

int main() {
    cc_server::testing::run_all_contract_tests();
    return cc_server::testing::g_test_stats().failed_tests > 0 ? 1 : 0;
}
