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
#include "cluster/cluster_connection.h"

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

// 声明 300MB 的帧（超过 kMaxPacketBytes=256MB）必须当场断链，且一条消息都不许投递。
// 旧实现在这里不是"不断链"，而是更糟：read_complete 返回 true 表示畸形，handle_read
// 于是叫起 decode_msg，后者按 300MB 去遍历参数区，而缓冲区里只有那 2KB 帧头 ——
// 测试进程会直接段错误，看不出是断言失败。
void test_bus_frame_oversized_disconnects() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    h.feed(make_header(300u * 1024u * 1024u, static_cast<uint16_t>(ClusterMsgType::kPing)));
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

// 一个 3MB 的写命令必须还能发出去。
//
// 本仓库把被复制的写命令也塞进总线帧（cluster_connection.cpp 的 kRepData ←
// replication_mgr 的 send_command_to_node），所以 SET 一个 3MB 的 value 就是一个 3MB
// 的帧。这条用例是给"把单帧上限往小里收"这个念头的刹车：接收端的 type 是发件人自报的，
// 没法按类型给数据帧单独开额度，上限一旦小于某些合法 value，大 value 的复制就会被静默
// 拒绝——测试与一致性检查都发现不了，因为副本只是少收了一条命令。
void test_bus_send_still_allows_large_replicated_value() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kRepData);
    msg.args.emplace_back("SET", 3);
    msg.args.emplace_back("big_key", 7);
    msg.args.emplace_back(std::string(3u * 1024u * 1024u, 'v'));

    EXPECT_TRUE(h.link.send_msg(msg));
    EXPECT_TRUE(h.link.is_connected());
}

// 发送者身份校验的判据本身。
//
// 注意覆盖面：这里证明的是谓词逻辑，`handle_link_msg` 是否真的在每个分支上叫它，
// 由集群 e2e（failover / psync / cluster-stress / cluster-full）反过来保证——那些脚本
// 里节点绑的都是 127.0.0.1、声称的也是 127.0.0.1，一旦这条校验写宽或写严，MEET/PING
// 就组不成集群，daily 的 e2e Job 会直接红。
void test_bus_sender_identity_predicate() {
    TEST_SUITE("Cluster Bus Sender Identity");

    // 一致 → 接受
    EXPECT_TRUE(bus_sender_ip_matches_peer("127.0.0.1", "127.0.0.1"));
    EXPECT_TRUE(bus_sender_ip_matches_peer("10.0.3.7", "10.0.3.7"));

    // 不一致 → 拒。这就是伪造集群成员身份的形状：报文里自称另一个节点
    //（身份 key 与 PFAIL 计票单位），但包是从别的地址进来的
    EXPECT_TRUE(!bus_sender_ip_matches_peer("10.0.0.9", "127.0.0.1"));
    EXPECT_TRUE(!bus_sender_ip_matches_peer("127.0.0.1", "10.0.0.9"));

    // 带尾空格/大小写不同的写法也不放过：身份 key 必须精确匹配，
    // 否则 "127.0.0.1 " 就能绕过等值比较
    EXPECT_TRUE(!bus_sender_ip_matches_peer("127.0.0.1 ", "127.0.0.1"));
    EXPECT_TRUE(!bus_sender_ip_matches_peer("127.0.0.2", "127.0.0.1"));

    // 任何一侧为空都不通过：观察不到来源不等于免检
    EXPECT_TRUE(!bus_sender_ip_matches_peer("", ""));
    EXPECT_TRUE(!bus_sender_ip_matches_peer("127.0.0.1", ""));
    EXPECT_TRUE(!bus_sender_ip_matches_peer("", "127.0.0.1"));
}

// 滴包防护的可观测口径：一帧挂着多久才算过期。
//
// 空闲超时挡不住滴包——每滴一个字节都会刷新 last_recv_time_，于是超时永远不触发，
// 而这条链路的接收缓冲一路往 kMaxPacketBytes(256MB) 涨；总线链路数不设上限，就是
// 一个不做认证的远程内存放大。期限不能是一个固定值：kMaxPacketBytes 留那么大本来
// 就是为了大 value 的复制帧（见 cluster_link.h 的注释），固定 30 秒会误杀慢链路。
// 所以期限是"固定宽限 + 声明长度折算的时间"，下限速率 1MB/s。
void test_bus_partial_frame_drip_times_out() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    constexpr uint32_t kDeclared = 200u * 1024u * 1024u;
    constexpr size_t kHead = sizeof(ClusterMsgHeader);
    h.feed(make_header(kDeclared, static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();  // 生产路径在这里起算
    EXPECT_TRUE(!h.disconnected);
    EXPECT_EQ(h.delivered, 0);

    const uint64_t now = ClusterLink::steady_now_ms();
    // 200MB / 1MBps = 200 秒，加 10 秒宽限。一分钟不算过期，慢链路不会被误杀。
    EXPECT_TRUE(!h.link.partial_frame_expired(kDeclared, kHead + 1, now + 60000));
    // 滴包（每 10 秒 1 字节）在 211 秒时必须过期。
    EXPECT_TRUE(h.link.partial_frame_expired(kDeclared, kHead + 2, now + 211000));
}

// 过期之后必须真的把链路断掉——只判过期不断链等于没防护。
void test_bus_partial_frame_timeout_disconnects() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    constexpr uint32_t kDeclared = 200u * 1024u * 1024u;
    h.feed(make_header(kDeclared, static_cast<uint16_t>(ClusterMsgType::kPing)));
    h.link.handle_read();
    EXPECT_TRUE(!h.disconnected);

    // 不需要真等两百多秒：把这帧的起算时刻往前挪，再滴一个字节
    h.link.set_partial_frame_start_for_test(ClusterLink::steady_now_ms() - 300000);
    EXPECT_EQ(::write(h.peer_fd, "x", 1), static_cast<ssize_t>(1));
    h.link.handle_read();

    EXPECT_TRUE(h.disconnected);
    EXPECT_EQ(h.delivered, 0);
}

// Channel 是按"登记那一刻的 fd"存进 link_channels_ 的。断开会把 fd_ 置成 -1，
// 而注销发生在断开回调里——那里必须还能拿到原来那个号码，否则 find(-1) 不命中，
// Channel 既不从 epoll 摘除也不 delete，fd 号被下一条链路复用后新链路根本进不了
// epoll（EPOLL_CTL_MOD 报 ENOENT），该节点的 gossip/复制静默停摆。
void test_disconnect_still_reports_the_registered_fd() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    int sv[2] = {-1, -1};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    ClusterLink link("peer-node", "127.0.0.1", 19000);
    link.set_fd(sv[0]);
    const int registered = link.fd();
    EXPECT_TRUE(registered >= 0);
    EXPECT_EQ(link.registered_fd(), registered);

    link.disconnect_and_notify();
    EXPECT_EQ(link.fd(), -1);              // 现在关掉了
    EXPECT_EQ(link.registered_fd(), registered);  // 注销 Channel 要用的是这个

    ::close(sv[1]);
}
void run_all_cluster_bus_framing_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running Cluster Bus Framing Tests\n";
    std::cout << "========================================\n\n";

    test_bus_frame_valid_is_delivered();
    test_bus_frame_oversized_disconnects();
    test_bus_frame_absurd_length_does_not_overread();
    test_bus_frame_shorter_than_header_disconnects();
    test_bus_send_still_allows_large_replicated_value();
    test_bus_sender_identity_predicate();
    test_bus_partial_frame_drip_times_out();
    test_bus_partial_frame_timeout_disconnects();
    test_disconnect_still_reports_the_registered_fd();

    std::cout << "\n========================================\n";
    std::cout << "All Cluster Bus Framing Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
