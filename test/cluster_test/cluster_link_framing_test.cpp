// 集群总线帧边界的单元测试。
//
// 总线端口是一个不做认证的输入面，任何"照声明长度去读"的路径都必须先证明
// 缓冲区里真有那么多字节。下面每条都手工造一个 header，直接喂给 handle_read()，
// 不依赖起真集群。

#include <unistd.h>
#include <sys/socket.h>

#include <cstring>
#include <iostream>
#include <string>

#include "../trace/test_assertions.h"
#include "cluster/cluster_link.h"

namespace cc_server {
namespace testing {

namespace {

    // 造一个 magic 正确、其余字段按需指定的帧头
    ClusterMsgHeader make_header(uint32_t declared_length, uint16_t msg_type) {
        ClusterMsgHeader header;
        std::memset(&header, 0, sizeof(header));
        header.magic = 0x43;  // 'C'
        header.version = 1;
        header.type = msg_type;
        header.length = declared_length;
        std::snprintf(header.sender_name, sizeof(header.sender_name), "%s", "127.0.0.1:19000");
        return header;
    }

    // 一条链路 + 一对 socketpair：peer_fd 是给测试用的对端，link 持有另一端
    struct BusHarness {
        int link_fd = -1;
        int peer_fd = -1;
        bool disconnected = false;
        int delivered = 0;
        ClusterLink link;

        BusHarness()
            : link("peer-node", "127.0.0.1", 19000) {
            int sv[2] = {-1, -1};
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
                link_fd = sv[0];
                peer_fd = sv[1];
                link.set_fd(link_fd);
                link.set_disconnect_callback(
                    [this](const std::string&, ClusterLink*) { disconnected = true; });
                link.set_msg_callback([this](ClusterMsg&&, ClusterLink*) { delivered++; });
            }
        }

        ~BusHarness() {
            if (peer_fd >= 0) ::close(peer_fd);
            // link_fd 由 ClusterLink 自己负责（断链时已经关掉）
        }

        BusHarness(const BusHarness&) = delete;
        BusHarness& operator=(const BusHarness&) = delete;

        bool ok() const { return link_fd >= 0 && peer_fd >= 0; }

        void feed(const ClusterMsgHeader& header) {
            const ssize_t written = ::write(peer_fd, &header, sizeof(header));
            EXPECT_EQ(written, static_cast<ssize_t>(sizeof(header)));
        }
    };

}  // namespace

// 合法帧（长度正好等于 header）必须被投递，且链路保持连接
void test_bus_frame_valid_is_delivered() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);  // socketpair 都建不出来，这个环境不能算通过
        return;
    }

    h.feed(make_header(static_cast<uint32_t>(sizeof(ClusterMsgHeader)),
                       static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();

    EXPECT_EQ(h.delivered, 1);
    EXPECT_TRUE(!h.disconnected);
    EXPECT_TRUE(h.link.is_connected());
}

// 声明 3MB 的帧必须当场断链。旧上限是 256MB，这种帧会被当成"还没收完"一直挂着，
// 攻击者只要声明大帧再慢慢滴，就能按链路数量线性地把内存吃掉。
void test_bus_frame_oversized_disconnects() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    h.feed(make_header(3u * 1024u * 1024u, static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();

    EXPECT_TRUE(h.disconnected);
    EXPECT_EQ(h.delivered, 0);
    EXPECT_TRUE(!h.link.is_connected());
}

// 这一条是真正的越界读：旧实现让 read_complete() 对超长帧"返回 true 走断链"，
// 而 handle_read 于是叫起 decode_msg()，后者按 header.length 算出 args_size 就去
// 遍历参数区——缓冲区里只有一个 2KB 的帧头。length=0xFFFFFFFF 时它会想读 4GB，
// 进程当场段错误。修完之后畸形帧由 frame_invalid_ 表达，根本不会叫起 decode。
void test_bus_frame_absurd_length_does_not_overread() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    h.feed(make_header(0xFFFFFFFFu, static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();

    EXPECT_TRUE(h.disconnected);
    EXPECT_EQ(h.delivered, 0);
}

// length 小到装不下 header：必须断链，且不能把 loop 线程拖进
// while(read_complete()) 的死循环（P0-1 的原始症状）。
// 说明：这条在修复前后都会通过，它的作用是把"死循环不再回来"钉住，
// 不是用来证明本次改动。
void test_bus_frame_shorter_than_header_disconnects() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    h.feed(make_header(10u, static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();   // 返回即为"没有死循环"，ctest 的 TIMEOUT 兜底

    EXPECT_TRUE(h.disconnected);
    EXPECT_EQ(h.delivered, 0);
}

// 发送端与接收端的上限必须对称：自己不能发出会被对端判为畸形并断链的帧。
void test_bus_send_refuses_frame_over_the_limit() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kPing);
    // 三个 1MB 的参数：合计 > 2MB 上限
    msg.args.emplace_back(1024u * 1024u, 'a');
    msg.args.emplace_back(1024u * 1024u, 'b');
    msg.args.emplace_back(1024u * 1024u, 'c');

    // 旧代码把长度累加进 uint32 的 header.length 并不设上限；
    // 现在必须在发送之前就拒绝
    EXPECT_TRUE(!h.link.send_msg(msg));
    EXPECT_TRUE(h.link.is_connected());   // 拒绝发送不该顺带把链路搞断
}

void run_all_cluster_bus_framing_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Cluster Bus Framing Tests\n";
    std::cout << "========================================\n\n";

    test_bus_frame_valid_is_delivered();
    test_bus_frame_oversized_disconnects();
    test_bus_frame_absurd_length_does_not_overread();
    test_bus_frame_shorter_than_header_disconnects();
    test_bus_send_refuses_frame_over_the_limit();

    std::cout << "\n========================================\n";
    std::cout << "All Cluster Bus Framing Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
