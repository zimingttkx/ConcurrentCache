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
#include "cluster/cluster_bus.h"
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
        ClusterMsg last;  // 最近一次投递的消息（args 用来验参数帧）
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
                link.set_msg_callback([this](ClusterMsg&& m, ClusterLink*) {
                    last = std::move(m);
                    delivered++;
                });
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

        // 整帧（header + 参数区）一次喂进去
        void feed_frame(ClusterMsgHeader header, const std::string& payload) {
            header.magic = 0x43;
            header.length = static_cast<uint32_t>(sizeof(header) + payload.size());
            std::snprintf(header.sender_name, sizeof(header.sender_name), "%s", "127.0.0.1:19000");
            std::string bytes;
            bytes.append(reinterpret_cast<const char*>(&header), sizeof(header));
            bytes += payload;
            const ssize_t written = ::write(peer_fd, bytes.data(), bytes.size());
            EXPECT_EQ(written, static_cast<ssize_t>(bytes.size()));
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

    using enum BusMsgPlane;

    // ── 控制面 ──
    // 两侧都是字面地址且一致 → 接受
    EXPECT_TRUE(bus_sender_identity_accepted("127.0.0.1", "127.0.0.1", false, kControl));
    EXPECT_TRUE(bus_sender_identity_accepted("10.0.3.7", "10.0.3.7", true, kControl));

    // 两侧都是字面地址但不一致 → 拒。这就是伪造集群成员身份的形状：报文里自称
    // 另一个节点（身份 key 与 PFAIL 计票单位），但包是从别的地址进来的
    EXPECT_TRUE(!bus_sender_identity_accepted("10.0.0.9", "127.0.0.1", false, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.1", "10.0.0.9", true, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.2", "127.0.0.1", false, kControl));

    // 任一侧是主机名 → 不做地址比对。#45 原来在这里也要求逐字相等，于是 hostname /
    // Docker service-name / NAT 之后的部署里"对端自报是谁"与"链路在哪"永远不等：
    // 用主机名敲的那条 CLUSTER MEET 组不起集群，而且连带把复制数据面一起拦掉。
    EXPECT_TRUE(bus_sender_identity_accepted("127.0.0.1", "localhost", false, kControl));
    EXPECT_TRUE(bus_sender_identity_accepted("node-1", "10.0.0.5", false, kControl));
    EXPECT_TRUE(bus_sender_identity_accepted("db.internal", "db.internal", false, kControl));

    // 带尾空格/纯数字这类既不是地址也不是名字的写法一律拒：身份 key 必须精确匹配，
    // 否则 "127.0.0.1 " 就能绕过等值比较
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.1 ", "127.0.0.1", true, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("42", "42", true, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("bad_name", "127.0.0.1", true, kControl));

    // 任何一侧为空都不通过：观察不到来源不等于免检
    EXPECT_TRUE(!bus_sender_identity_accepted("", "", false, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.1", "", false, kControl));
    EXPECT_TRUE(!bus_sender_identity_accepted("", "127.0.0.1", true, kControl));

    // ── 数据面（复制命令 / REPLSYNC）──
    // 地址不作为凭据，看的是"这个名字在不在成员表里"。已在表里的成员即使隔着 NAT
    // 也必须放行——旧实现在这里会静默丢掉复制数据，而且协议面上看不出任何东西。
    EXPECT_TRUE(bus_sender_identity_accepted("10.0.0.9", "127.0.0.1", true, kData));
    EXPECT_TRUE(bus_sender_identity_accepted("node-1", "10.0.0.5", true, kData));
    // 不在表里的名字想直接推复制命令进来：拒（握手阶段不会发数据面报文，所以
    // "先认识"这个前提成立）
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.1", "127.0.0.1", false, kData));
    EXPECT_TRUE(!bus_sender_identity_accepted("10.0.0.9", "127.0.0.1", false, kData));
    // 就算名字在表里，畸形写法也不能过
    EXPECT_TRUE(!bus_sender_identity_accepted("127.0.0.1 ", "127.0.0.1", true, kData));
    EXPECT_TRUE(!bus_sender_identity_accepted("", "127.0.0.1", true, kData));
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
// 只滴帧头的一部分，也不能让链路无限期挂着。
//
// 记账只看"整帧还差多少"的话，攻击者不必声明一个巨大的帧——每次只滴几个字节、
// 永远凑不满 2120 字节的头，就能让每一轮的起算时间都从零开始：缓冲确实涨不动，
// 但这条链路和它的 fd 可以无限挂着（心跳与连接数照吃）。所以"头没读全"同样按
// 一帧没着落来记账，期限按头长折算 = 固定 10 秒宽限。
void test_bus_partial_header_also_times_out() {
    TEST_SUITE("Cluster Bus Frame Bounds");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    const char junk[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(::write(h.peer_fd, junk, sizeof(junk)), static_cast<ssize_t>(sizeof(junk)));
    h.link.handle_read();
    EXPECT_TRUE(!h.disconnected);  // 才 10 个字节，宽限内

    h.link.set_partial_frame_start_for_test(ClusterLink::steady_now_ms() - 11000);
    EXPECT_EQ(::write(h.peer_fd, "x", 1), static_cast<ssize_t>(1));
    h.link.handle_read();
    EXPECT_TRUE(h.disconnected);   // 11 秒没有整帧头 -> 断链

    if (!h.disconnected) {
        // 没断的话把这条链关掉，别让 socketpair 泄漏到下一个用例
        h.link.disconnect_and_notify();
    }
}
// v2 参数帧必须把任意字节的参数原样带回来。
//
// 这条是 #37 的正面判据：老框架把参数用裸 0xC0 连接且没有转义，所以一个 value
// 里只要含 0xC0，副本端收到的就是被切错位的参数——不报错、不重试、日志里也看不出来，
// 表现为主从静默发散。这里刻意混入 0xC0、'\n'、'\r' 和一个空参数。
void test_bus_v2_frame_roundtrips_binary_args() {
    TEST_SUITE("Cluster Bus Framing v2");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    const std::vector<std::string> args = {
        "RESTORE",
        std::string("k") + static_cast<char>(0xC0) + "\n\r",
        std::string(1, static_cast<char>(0xC0)),
        std::string(),
        "REPLACE",
    };

    std::string payload;
    bus_args_encode(args, payload);
    EXPECT_EQ(payload.size(), bus_args_frame_bytes(args));

    ClusterMsgHeader header;
    std::memset(&header, 0, sizeof(header));
    header.version = kBusFramingVersion;
    header.type = static_cast<uint16_t>(ClusterMsgType::kRepData);
    h.feed_frame(header, payload);
    h.link.handle_read();

    EXPECT_EQ(h.delivered, 1);
    EXPECT_TRUE(!h.disconnected);
    EXPECT_EQ(h.last.args.size(), args.size());
    EXPECT_TRUE(h.last.args == args);
}

// 编码侧算的长度和真实写出去的字节数必须一致：header.length 一旦小于实际帧长，
// 接收端会把一条帧当两条解（后半截被认成坏 magic），大于则永远等不齐而卡死。
void test_bus_v2_frame_length_matches_encoder() {
    TEST_SUITE("Cluster Bus Framing v2");

    std::vector<std::string> args;
    for (int i = 0; i < 300; ++i) {
        args.emplace_back(static_cast<size_t>(i % 17) + 1, static_cast<char>('a' + i % 26));
    }
    // 用与实现无关的方式算一遍帧长（std::to_string 的位数），两边对不上就是
    // 编码侧或 header.length 侧算错了
    const auto digits = [](uint64_t v) { return std::to_string(v).size(); };
    size_t expected = digits(args.size()) + 1;
    for (const auto& a : args) {
        expected += digits(a.size()) + 1 + a.size();
    }
    std::string payload;
    bus_args_encode(args, payload);
    EXPECT_EQ(payload.size(), expected);
    EXPECT_EQ(bus_args_frame_bytes(args), expected);

    std::vector<std::string> back;
    std::string err;
    EXPECT_TRUE(bus_args_decode(payload.data(), payload.size(), back, err));
    EXPECT_TRUE(back == args);

    // 少一个字节：最后一圈的载荷不完整 → 必须判失败，不能交出一个截断的参数
    back.clear();
    EXPECT_TRUE(!bus_args_decode(payload.data(), payload.size() - 1, back, err));
    // count 与实际条数不符（300 写成 200）：多出来的字节是"尾随字节"，同样判失败
    std::string tampered = payload;
    tampered.replace(0, 3, "200");
    back.clear();
    EXPECT_TRUE(!bus_args_decode(tampered.data(), tampered.size(), back, err));
}

// 版本号为其它值时不能"挑一套规则试试"：任何一套都会解出错位参数并被下游执行，
// 所以必须当场断链。
void test_bus_unknown_framing_version_disconnects() {
    TEST_SUITE("Cluster Bus Framing v2");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    ClusterMsgHeader header;
    std::memset(&header, 0, sizeof(header));
    header.version = 7;  // 既不是 v1 也不是 v2
    header.type = static_cast<uint16_t>(ClusterMsgType::kRepData);
    h.feed_frame(header, "2\n3\nSET\n3\nabc");
    h.link.handle_read();

    EXPECT_EQ(h.delivered, 0);
    EXPECT_TRUE(h.disconnected);
}

// 升级窗口内的兼容路径：v1 帧仍按 0xC0 切分。gossip 的参数是 ASCII，
// 这条分支只为让还没重启完的老节点继续参与控制面。
void test_bus_v1_legacy_frame_still_parses() {
    TEST_SUITE("Cluster Bus Framing v2");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    ClusterMsgHeader header;
    std::memset(&header, 0, sizeof(header));
    header.version = kBusFramingLegacyVersion;
    header.type = static_cast<uint16_t>(ClusterMsgType::kPing);
    h.feed_frame(header, std::string("ping") + static_cast<char>(0xC0) + "pong" + static_cast<char>(0xC0));
    h.link.handle_read();

    EXPECT_EQ(h.delivered, 1);
    EXPECT_TRUE(!h.disconnected);
    EXPECT_EQ(h.last.args.size(), static_cast<size_t>(2));
    EXPECT_TRUE(h.last.args.size() == 2 && h.last.args[0] == "ping" && h.last.args[1] == "pong");
}

// 发送侧写进 socket 的字节，必须能被接收侧按 header 里声明的版本解回来。
//
// 前面几条是"手工造帧喂进去"，这条反过来：让 send_msg 自己写，再从对端读原始字节。
// header.length 与真实帧长脱钩、或者 version 没跟着实际用的框架走，只有这条路能发现。
void test_bus_send_msg_writes_a_decodable_v2_frame() {
    TEST_SUITE("Cluster Bus Framing v2");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    ClusterMsg msg;
    msg.header.type = static_cast<uint16_t>(ClusterMsgType::kRepData);
    msg.args.emplace_back("SET");
    msg.args.emplace_back("bin_key");
    msg.args.emplace_back(std::string(1, static_cast<char>(0xC0)) + "tail");

    EXPECT_TRUE(h.link.send_msg(msg));
    h.link.handle_write();

    auto read_exact = [&](size_t want) -> std::string {
        std::string got;
        char buf[4096];
        while (got.size() < want) {
            // 一次最多只读"还缺多少"：否则会把正文一起吸进来，下一个 read_exact
            // 就读到了不属于它的数据（CI 上第一次跑就是这样红的：读了 2143 而不是 2120）
            const size_t need = want - got.size();
            const size_t room = need < sizeof(buf) ? need : sizeof(buf);
            const ssize_t n = ::read(h.peer_fd, buf, room);
            if (n <= 0) break;
            got.append(buf, static_cast<size_t>(n));
        }
        return got;
    };

    const std::string head_bytes = read_exact(sizeof(ClusterMsgHeader));
    EXPECT_EQ(head_bytes.size(), sizeof(ClusterMsgHeader));
    if (head_bytes.size() != sizeof(ClusterMsgHeader)) return;

    ClusterMsgHeader header;
    std::memcpy(&header, head_bytes.data(), sizeof(header));
    EXPECT_EQ(header.version, kBusFramingVersion);
    EXPECT_TRUE(header.length >= sizeof(ClusterMsgHeader));
    if (header.length < sizeof(ClusterMsgHeader)) return;

    const std::string payload = read_exact(header.length - sizeof(ClusterMsgHeader));
    EXPECT_EQ(payload.size(), static_cast<size_t>(header.length) - sizeof(ClusterMsgHeader));

    std::vector<std::string> back;
    std::string err;
    EXPECT_TRUE(bus_args_decode(payload.data(), payload.size(), back, err));
    EXPECT_TRUE(back == msg.args);
}

// fd 错误这一条路必须把"链路没了"通知给持有者。
//
// 写成 disconnect()（不触发回调）时，ClusterBus::remove_link 不会被叫起：
// links_ 里那条 handshake:* 条目、它的 Channel 注册、link_channels_ 里的对象
// 三者永久留着。更糟的是 fd 已被 close 并可被下一条连接复用，之后清理这条死
// 条目时用的是它的 registered_fd() —— 那正是新链路的 fd，于是把活链路的 Channel
// 从 epoll 里摘掉：活链路从此收不到事件，表现成"节点之间偶发永久失联"。
void test_bus_link_error_path_notifies_owner() {
    TEST_SUITE("Cluster Bus Link Lifecycle");

    BusHarness h;
    if (!h.ok()) {
        EXPECT_TRUE(false);
        return;
    }

    h.link.handle_error();

    EXPECT_TRUE(h.disconnected);
    EXPECT_EQ(h.delivered, 0);
    EXPECT_TRUE(!h.link.is_connected());

    // 第二次调用不能再通知一次（remove_link 被叫两次会误摘别的 fd）
    h.disconnected = false;
    h.link.handle_error();
    EXPECT_TRUE(!h.disconnected);
}

// 入站配额判断本身。总线端口不认证，反复连进来就能把链路数推高，
// 直到 fd 耗尽让 accept 走非 EAGAIN 分支、事件循环开始空转。
void test_bus_inbound_quota_predicate() {
    TEST_SUITE("Cluster Bus Link Lifecycle");

    using Bus = ClusterBus;

    EXPECT_TRUE(Bus::inbound_admitted(0, 8));
    EXPECT_TRUE(Bus::inbound_admitted(7, 8));
    // 正好到配额则拒：判据是 <，写成 <= 会让配额多一条并且这条用例变红
    EXPECT_TRUE(!Bus::inbound_admitted(8, 8));
    EXPECT_TRUE(!Bus::inbound_admitted(9, 8));
    EXPECT_TRUE(Bus::inbound_admitted(Bus::kMaxInboundLinks - 1));
    EXPECT_TRUE(!Bus::inbound_admitted(Bus::kMaxInboundLinks));
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
    test_bus_v2_frame_roundtrips_binary_args();
    test_bus_v2_frame_length_matches_encoder();
    test_bus_unknown_framing_version_disconnects();
    test_bus_v1_legacy_frame_still_parses();
    test_bus_send_msg_writes_a_decodable_v2_frame();
    test_bus_link_error_path_notifies_owner();
    test_bus_inbound_quota_predicate();
    test_bus_partial_frame_drip_times_out();
    test_bus_partial_frame_timeout_disconnects();
    test_disconnect_still_reports_the_registered_fd();
    test_bus_partial_header_also_times_out();

    std::cout << "\n========================================\n";
    std::cout << "All Cluster Bus Framing Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
