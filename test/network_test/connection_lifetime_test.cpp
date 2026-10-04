// 连接与事件循环的生命周期测试
//
// 这一层以前没有任何单元测试：Connection 的关闭时序、EventLoop 的跨线程任务队列
// 都只能靠起真服务器去猜。下面两条把这两处时序直接钉住，改坏了会立刻红。

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
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

// EPOLLHUP 不能被静默吞掉。
//
// 旧接线里 Connection 只给 Channel 设了 read / write / error 三个回调，而 Channel 的
// HUP 分支写的是"有 close_cb 就调它，然后 return"。close_cb 从来没被设过，于是 HUP
// 到达时**什么都不做**就返回：fd 既不读也不关。而 EPOLLHUP 是持续条件，epoll_wait
// 每轮都会立刻返回同一个事件 —— EventLoop 变成 100% CPU 空转，客户端永远等不到回复。
//
// 这里不起 loop，直接把事件喂给 handle_event()，所以时序是钉死的、不靠调度。
void test_hup_alone_still_finishes_the_connection() {
    TEST_SUITE("Connection HUP Handling");

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    EventLoop loop;
    auto owner = std::make_unique<Connection>(sv[0], &loop);

    bool notified = false;
    owner->set_close_callback([&notified]() { notified = true; });

    // 对端留下一条完整命令后整个关闭：数据还没被读，HUP 已经成立
    const std::string cmd = "*1\r\n$4\r\nPING\r\n";
    EXPECT_EQ(::write(sv[1], cmd.data(), cmd.size()), static_cast<ssize_t>(cmd.size()));
    ::close(sv[1]);

    owner->channel()->set_triggered_events(EPOLLHUP);
    owner->channel()->handle_event();

    // 必须有人接手这条已经断掉的连接
    EXPECT_TRUE(notified);

    // 收尾走 Connection::close()：本用例的 close_callback_ 只置标志、不销毁对象，
    // 而 fd 的所有权在 Connection 手里。这里要是直接 ::close(sv[0])，出作用域时
    // ~Socket 会对同一个号码再关一次——同一个二进制里后面还有别的用例，那个号
    // 可能已经被它们重新 open 出来了。
    owner->close();
}

// 契约：一条任务抛异常，不能连累排在它后面的任务，更不能穿出 loop()。
//
// loop() 是 SubReactor / MainReactor 线程的入口，那条调用链上没有任何 catch，
// 队列里一个异常逃到线程顶端就是 std::terminate。
//
// 这里不起线程也不睡眠：先投一条会抛的、再投一条计数的，然后才 quit() + loop()。
// quit_ 已经置位，循环第一圈就 break，唯一的执行点是退出路径上那次 drain
// （#67 加的），所以时序是钉死的：
//   - 修复前：异常从 drain_pending_tasks 穿出 loop()，escaped 为真，红；
//   - 修复后：escaped 为假，且排在后面的那条照常执行完。
void test_throwing_task_does_not_stop_the_others() {
    TEST_SUITE("EventLoop Task Isolation");

    EventLoop loop;
    std::atomic<int> ran{0};

    loop.queue_in_loop([]() { throw std::runtime_error("boom"); });
    loop.queue_in_loop([&ran]() { ran.fetch_add(1); });

    loop.quit();
    bool escaped = false;
    try {
        loop.loop();
    } catch (const std::exception&) {
        escaped = true;
    }

    EXPECT_TRUE(!escaped);
    EXPECT_EQ(ran.load(), 1);
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
    test_hup_alone_still_finishes_the_connection();
    test_throwing_task_does_not_stop_the_others();

    std::cout << "\n========================================\n";
    std::cout << "All Connection Lifetime Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
