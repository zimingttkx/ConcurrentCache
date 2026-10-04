// 连接与事件循环的生命周期测试
//
// 这一层以前没有任何单元测试：Connection 的关闭时序、EventLoop 的跨线程任务队列
// 都只能靠起真服务器去猜。下面两条把这两处时序直接钉住，改坏了会立刻红。

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "../trace/test_assertions.h"
#include "base/config.h"
#include "network/connection.h"
#include "network/event_loop.h"

namespace cc_server {
namespace testing {

namespace {
    /// @brief 把两条高水位压到下限（1MB），让用例不必真灌几十 MB
    void use_low_buffer_watermark() {
        const std::string path = "/tmp/cc_buffer_limit.conf";
        {
            std::ofstream out(path, std::ios::trunc);
            out << "client_query_buffer_limit = 1048576\n";
            out << "client_output_buffer_limit = 1048576\n";
        }
        EXPECT_TRUE(Config::instance().load(path));
        std::remove(path.c_str());
    }
}  // namespace

// 契约：Connection::close() 必须先关掉自己的 fd，再通知所有者。
//
// 所有者收到的回调就是 SubReactor::remove_connection——它 erase 掉 map 里唯一的
// unique_ptr，回调返回时 this 已经析构。旧顺序（先回调、后 client_socket_.close()）
// 因此在已释放的内存上再读一次 fd_：单线程下读到 ~Socket 写的 -1，是无害的空操作；
// 那块内存被下一个 Connection 复用时，关掉的就是新连接的 fd。
void test_connection_close_frees_fd_before_notifying_owner() {
    TEST_SUITE("Connection Close Lifetime");

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const int peer_fd = sv[1];  // 保持打开，让本用例只操作 sv[0] 这一端

    EventLoop loop;
    auto owner = std::make_unique<Connection>(sv[0], &loop);
    const int conn_fd = owner->fd();
    EXPECT_TRUE(conn_fd >= 0);

    // 回调只做两件事：记录 fd 此刻的状态，然后把唯一所有者摘走。
    // reset() 放在最后一句——回调本身也住在这个对象里，摘走之后不能再读捕获。
    int fd_state_at_callback = -2;
    owner->set_close_callback([&owner, &fd_state_at_callback, conn_fd]() {
        fd_state_at_callback = ::fcntl(conn_fd, F_GETFD);
        owner.reset();
    });

    owner->close();

    // fd 必须在通知所有者之前就关掉
    EXPECT_EQ(fd_state_at_callback, -1);
    // 关掉的是自己那一条，不是别人的：三个标准 fd 必须还在
    EXPECT_TRUE(::fcntl(0, F_GETFD) != -1);
    EXPECT_TRUE(::fcntl(1, F_GETFD) != -1);
    EXPECT_TRUE(::fcntl(2, F_GETFD) != -1);

    ::close(peer_fd);
}

// 契约：queue_in_loop 投递的任务由 loop() 线程执行。
//
// 客户端连接的登记整体走这条队列（SubReactor::add_connection 不再在 accept
// 线程里碰 epoll），所以"排进去的任务一定会被执行"是连接能不能通的前提。
void test_event_loop_runs_queued_tasks() {
    TEST_SUITE("EventLoop Pending Tasks");

    EventLoop loop;
    std::atomic<int> executed{0};

    loop.queue_in_loop([&executed]() { executed.fetch_add(1); });
    // loop 线程还没跑起来，任务只能排队，不能就地执行
    EXPECT_EQ(executed.load(), 0);

    std::thread loop_thread([&loop]() { loop.loop(); });

    bool drained = false;
    for (int i = 0; i < 200 && !drained; ++i) {
        drained = executed.load() == 1;
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(drained);

    loop.quit();
    loop_thread.join();
}

// 契约：输入缓冲不能无限增长。
//
// 一条客户端只要发个永不写完的 RESP bulk（"*1\r\n$2000000\r\n" 然后不再发），
// 服务端就会把这堆没用的字节一直攒在 input_buffer_ 里。没有高水位时，几个这样的
// 连接就足以把进程撑爆。
void test_input_buffer_high_water_closes_client() {
    TEST_SUITE("Connection Buffer High Water");

    use_low_buffer_watermark();

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    EventLoop loop;
    auto owner = std::make_unique<Connection>(sv[0], &loop);

    bool closed_by_server = false;
    owner->set_close_callback([&closed_by_server]() { closed_by_server = true; });

    const std::string header = "*1\r\n$2000000\r\n";
    EXPECT_EQ(::write(sv[1], header.data(), header.size()),
              static_cast<ssize_t>(header.size()));

    // handle_read() 每次只从 socket 取 4095 字节，所以按这个节奏喂到超过 1MB 上限
    std::vector<char> chunk(4096, 'a');
    size_t pushed = header.size();
    while (!closed_by_server && pushed < 4ull * 1024 * 1024) {
        const ssize_t written = ::write(sv[1], chunk.data(), chunk.size());
        EXPECT_TRUE(written > 0);
        pushed += static_cast<size_t>(written);
        owner->handle_read();
    }

    EXPECT_TRUE(closed_by_server);
    EXPECT_TRUE(owner->fd() < 0);

    owner->close();  // 幂等，确认重复关闭不会崩
    ::close(sv[1]);
}

// 契约：对端不读时，积压的回复不能无限增长。
void test_output_buffer_high_water_closes_client() {
    TEST_SUITE("Connection Buffer High Water");

    use_low_buffer_watermark();

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    EventLoop loop;
    auto owner = std::make_unique<Connection>(sv[0], &loop);

    bool closed_by_server = false;
    owner->set_close_callback([&closed_by_server]() { closed_by_server = true; });

    // 完全不从 sv[1] 读，让回复只进不出
    const std::string payload(64 * 1024, 'x');
    for (int i = 0; i < 64 && !closed_by_server; ++i) {
        owner->send_response(payload);
    }

    EXPECT_TRUE(closed_by_server);
    // 关掉之后继续排队必须是无效的，不能再把内存攒回去
    const std::string after_close(1024, 'y');
    owner->send_response(after_close);

    ::close(sv[1]);
}

// quit 之前投出去的任务必须被收尾，不能随循环一起被丢掉。
//
// 这里不用线程、不用睡眠：先把任务 queue_in_loop，再 quit()，然后才 loop()。
// 循环第一圈就看到 quit_ 直接 break，于是唯一可能执行它的地方就是退出路径上的
// 那次 drain —— 少那一次，任务就永久留在队列里，而它带着一个已经 accept 到的 fd。
void test_event_loop_drains_pending_tasks_on_exit() {
    TEST_SUITE("EventLoop Exit Drain");

    EventLoop loop;
    std::atomic<int> executed{0};

    loop.queue_in_loop([&executed]() { executed.fetch_add(1); });
    EXPECT_EQ(executed.load(), 0);  // 还没跑循环，只许排队

    loop.quit();
    loop.loop();                    // 立刻看到 quit_，不会进 epoll_wait

    EXPECT_EQ(executed.load(), 1);  // 退出路径把这条任务收尾了
}

// 同一次 pipeline 里，某条命令关掉连接之后，后面的命令不许再交给回调。
//
// 触发路径是真实存在的：#37 的输出缓冲高水位就在命令回调里调 close()。fd 已经关了、
// 表项也已经被 SubReactor 摘走，继续把剩下的命令喂进去就是在对一条已摘表的连接做
// 业务处理（回复会静默丢掉，看起来是"少回了几条"）。
void test_command_loop_stops_once_connection_is_closed() {
    TEST_SUITE("Connection Command Loop");

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    EventLoop loop;
    auto owner = std::make_unique<Connection>(sv[0], &loop);

    int delivered = 0;
    owner->set_command_callback([&delivered](const RespValue&, Connection* conn) {
        ++delivered;
        conn->close();  // 第一条命令就把这条连接关掉
    });

    const std::string pipelined = "*1\r\n$4\r\nPING\r\n" "*1\r\n$4\r\nPING\r\n";
    EXPECT_EQ(::write(sv[1], pipelined.data(), pipelined.size()),
              static_cast<ssize_t>(pipelined.size()));

    owner->handle_read();

    EXPECT_EQ(delivered, 1);
    EXPECT_TRUE(owner->fd() < 0);

    ::close(sv[1]);
}

void run_all_connection_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Connection / EventLoop Lifetime Tests\n";
    std::cout << "========================================\n\n";

    test_connection_close_frees_fd_before_notifying_owner();
    test_event_loop_runs_queued_tasks();
    test_input_buffer_high_water_closes_client();
    test_output_buffer_high_water_closes_client();
    test_event_loop_drains_pending_tasks_on_exit();
    test_command_loop_stops_once_connection_is_closed();

    std::cout << "\n========================================\n";
    std::cout << "All Connection Lifetime Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
