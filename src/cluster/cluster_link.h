// cluster_link.h
#ifndef CONCURRENTCACHE_CLUSTER_LINK_H
#define CONCURRENTCACHE_CLUSTER_LINK_H

#include "cluster_node.h"
#include "../network/buffer.h"
#include <memory>
#include <functional>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cc_server {

// 前向声明
struct GossipMsg;

// 集群消息类型
enum class ClusterMsgType : uint16_t {
    kPing = 1,       // PING 消息
    kPong = 2,       // PONG 消息（响应 PING）
    kMeet = 3,       // MEET 消息（请求加入集群）
    kMail = 4,       // 消息传递
    kFail = 5,       // 节点下线通知
    kPublish = 6,    // 发布消息
    kFailoverAuthReq = 7,  // 故障转移请求
    kFailoverAuthAck = 8,  // 故障转移确认
    kUpdate = 9,     // 节点信息更新
    kRepData = 10,   // 复制数据命令
    kPush = 11,      // 推送节点信息（与 GossipType::kPush 对应，用于 UPDATE 广播）
};

// 集群消息头
// 注意：length 字段为 uint32（含 header 的总字节数）。
// 修复 P0-1：此前为 uint16，复制大 value（>64KB）时发送端累加回绕截断，
// 接收端提前判定消息完整，导致后续字节流永久错位。
struct ClusterMsgHeader {
    uint32_t magic;           // 消息标识 (0x43 = 'C')
    uint16_t version;         // 协议版本
    uint16_t type;            // 消息类型
    uint32_t length;          // 消息总长度（含 header，单位字节）
    uint64_t sender_epoch;    // 发送者 epoch
    char sender_name[40];     // 发送者节点名
    uint16_t flags;           // 发送者标志
    uint16_t port;            // 发送者端口
    uint32_t state;           // 集群状态
    // 槽位图：每个 bit 表示一个槽是否属于发送者（预留字段，用于未来优化）
    // 当前使用 GossipNodeInfo::used_slot 动态传递槽信息
    uint8_t slot_map[16384 / 8]; // 槽位图 (2048 bytes)
};

// 参数区帧格式的协议版本。
//
// v1 把多个参数用裸 0xC0 字节拼起来且没有任何转义 —— 而复制/迁移的参数里
// 本来就允许出现任意字节（value、RESTORE 载荷都是二进制）。一个含 0xC0 的
// value 会被拆成两个参数，副本端于是执行一条错位命令或直接丢掉，主从静默发散。
// v2 换成"参数条数 + 每条 <字节数>\n<原始字节>"，与 CacheObject 的序列化框架同构。
//
// 版本写在头里，接收端按版本分支：v1 帧仍按老规则解（老节点的 gossip 参数是
// ASCII，不含 0xC0，所以升级窗口内控制面照常工作），v2 帧按新规则解，其它版本
// 一律判畸形并断链 —— 宁可断链也不要在解出一堆错位参数之后"看起来正常"。
// 反方向没有无损方案：老节点看不懂转义，任何带内转义都必须同时转义转义符本身。
// 因此混版本期间**数据面**（复制/迁移）不受支持，需要整集群一起升级。
constexpr uint16_t kBusFramingVersion = 2;
constexpr uint16_t kBusFramingLegacyVersion = 1;

// 集群总线消息
struct ClusterMsg {
    ClusterMsgHeader header;
    std::vector<std::string> args;  // 消息参数

    ClusterMsg() {
        memset(&header, 0, sizeof(header));
        header.magic = 0x43;  // 'C'
        header.version = kBusFramingVersion;
    }
};


// 参数区编码后的字节数（先算长度再一次性写，避免 send_msg 与接收端各算一套）。
size_t bus_args_frame_bytes(const std::vector<std::string>& args);

// v2：count + 每条 <字节数>\n<原始字节>。追加到 out 末尾。
void bus_args_encode(const std::vector<std::string>& args, std::string& out);

// v2 解码。任何一圈不完整、条数对不上、或参数区有余字节 → false 并填 err。
bool bus_args_decode(const char* data, size_t len, std::vector<std::string>& out,
                     std::string& err);

// v1 解码：按裸 0xC0 切分（不认转义，仅用于兼容尚未升级的对端）。
void bus_args_decode_legacy(const char* data, size_t len, std::vector<std::string>& out);

// ClusterLink 类：封装与另一个集群节点的 TCP 连接
class ClusterLink {
public:
    using MsgCallback = std::function<void(ClusterMsg&& msg, ClusterLink* link)>;
    using DisconnectCallback = std::function<void(const std::string& node_name, ClusterLink* link)>;

    ClusterLink(const std::string& node_name, const std::string& ip, int port);
    ~ClusterLink();

    ClusterLink(const ClusterLink&) = delete;
    ClusterLink& operator=(const ClusterLink&) = delete;

    // 连接管理
    bool connect();                      // 主动连接对端
    void disconnect();                   // 断开连接
    void set_fd(int fd) { fd_ = fd; registered_fd_ = fd; connected_.store(true); }  // 直接设置fd（入站连接用）
    [[nodiscard]] bool is_connected() const { return connected_.load(); }

    // 断开并通知（修复 P0-2 UAF）：
    // 1. 先置 disconnected（后续事件回调直接返回，不再触碰 this 的资源）
    // 2. 关闭 fd
    // 3. 调用 disconnect_callback_（回调可能销毁 this）
    // 4. 调用返回后绝不访问任何成员 —— 由调用方（handle_read/handle_write 的
    //    事件路径）在回调返回后立即 return
    void disconnect_and_notify();

    // 发送消息
    bool send_msg(const ClusterMsg& msg);
    bool send_ping();
    bool send_pong();
    bool send_meet(const std::string& my_ip, int my_port);
    bool send_gossip(const GossipMsg& msg);

    // 发送原始 RESP 命令数据（用于 MIGRATE 等场景）
    bool send_raw(const std::string& data);

    // 接收消息
    void handle_read();
    void handle_write();

    /**
     * @brief fd 错误时的收尾：记录 + disconnect_and_notify()
     *
     * 这个方法的唯一存在理由是历史上这里写成过 `disconnect()`（不触发回调），
     * 于是 ClusterBus 的 links_ 条目、它的 Channel 注册、link_channels_ 里的
     * Channel 对象三者都不回收：每来一次 EPOLLERR 就永久泄漏一条链路，而
     * registered_fd() 已被复用的话，之后清理这条死条目还会把**新链路**的
     * Channel 摘掉。收口成一个方法，判据就能被单元测试钉住（见
     * test/cluster_test/cluster_link_framing_test.cpp）。
     */
    void handle_error();

    // 属性访问
    [[nodiscard]] const std::string& node_name() const { return node_name_; }
    [[nodiscard]] const std::string& ip() const { return ip_; }
    [[nodiscard]] int port() const { return port_; }
    [[nodiscard]] int fd() const { return fd_; }

    /// @brief 这条链路当初登记进 EventLoop 时用的 fd，断开后仍然有效。
    ///
    /// Channel 是按"注册那一刻的 fd"存进 link_channels_ 的，而 disconnect 会把
    /// fd_ 置成 -1。断开回调里注销 Channel 时如果再去读 link->fd()，拿到的是 -1，
    /// `link_channels_.find(-1)` 必然未命中 —— 那条 Channel 既不会从 epoll 摘除，
    /// 也不会被 delete：留在 EventLoop::channels_ 里指向一个已经关掉的 fd，并且等
    /// 这个号码被下一条链路复用时，update_channel 走 EPOLL_CTL_MOD 拿到 ENOENT，
    /// 新链路就永远进不了 epoll，那个节点的 gossip/复制静默停摆。
    [[nodiscard]] int registered_fd() const { return registered_fd_; }

    // 回调设置
    void set_msg_callback(MsgCallback cb) { msg_callback_ = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { disconnect_callback_ = std::move(cb); }

    // 最后通信时间（用于超时检测）
    void update_last_recv_time();
    [[nodiscard]] int64_t last_recv_time() const { return last_recv_time_.load(); }

    // 半帧停留检测（滴包防护，理由见 kPartialFrameGraceMs 的注释）。
    // 记账与判过期是同一个函数，production（handle_read）与测试走的是同一条路，
    // 所以"超时真的会把链路断掉"这件事是可证的；时钟由调用方传入。
    bool partial_frame_expired(uint32_t declared_len, size_t buffered, uint64_t now_ms);
    [[nodiscard]] static uint64_t steady_now_ms();

    /// @brief 测试接缝：把这一帧的起算时刻往前挪，用来在没有真实几百秒的情况下
    /// 驱动 handle_read 里的超时断链分支（与 set_fd 同一性质）。
    void set_partial_frame_start_for_test(uint64_t started_ms);

private:
    // 完整读取一个消息
    bool read_complete();
    // 解码消息
    bool decode_msg();

    std::string node_name_;              // 对端节点名称
    std::string ip_;                     // 对端 IP
    int port_;                            // 对端端口
    int fd_ = -1;                        // socket fd

    std::atomic<bool> connected_{false};  // 连接状态
    std::atomic<int64_t> last_recv_time_{0};  // 最后接收时间

    Buffer send_buffer_;                 // 发送缓冲区
    std::mutex send_mutex_;              // 保护 send_buffer_ 的多线程访问
    Buffer recv_buffer_;                  // 接收缓冲区

    MsgCallback msg_callback_;            // 消息回调
    DisconnectCallback disconnect_callback_;  // 断开回调

    static constexpr uint32_t kMsgMagic = 0x43;  // 'C'
    static constexpr size_t kHeaderSize = sizeof(ClusterMsgHeader);
    /// @brief 单帧上限。
    ///
    /// 这个值不能按 Redis 的 CLUSTER_BUS_MAX_PACKET_SIZE（2MB）来收：本仓库把被复制的
    /// 写命令也塞进了总线帧（cluster_connection.cpp 的 kRepData → replication_mgr 的
    /// send_command_to_node），SET 一个 10MB 的 value 就是一个 10MB 的帧；而接收端不能
    /// 按 type 区分上限——type 和 sender_name 一样是发件人自报的，声明 kRepData 就能拿到
    /// 大额度。所以真要收这个数，得先把数据面从总线控制面拆出去（未在本仓库实现），
    /// 而不是在这里改一个数字。现在保持原值，本次只修"照声明长度越界读"的崩溃。
    static constexpr uint32_t kMaxPacketBytes = 256u * 1024u * 1024u;

    /// @brief 收到畸形帧（length 装不下 header，或超过单帧上限）时置位。
    /// 必须由 handle_read 单独判断并断链：不能让 decode_msg 拿着一个缓冲区里
    /// 并不存在的长度去读参数区。
    bool frame_invalid_ = false;

    /// @brief 半帧允许停留多久：固定宽限 + 按声明长度折算的最低传输速率。
    ///
    /// 空闲检测（last_recv_time_ + ping_timeout）挡不住滴包：对端先送一个 length =
    /// kMaxPacketBytes-1 的合法帧头，之后每 10 秒滴 1 个字节——每个字节都会刷新
    /// last_recv_time_，于是超时永远不触发，而这条链路的接收缓冲一路涨到接近 256MB，
    /// 总线链路数又不设上限。
    ///
    /// 只给一个固定值（比如 30 秒）会误杀慢链路：kMaxPacketBytes 之所以留那么大，
    /// 就是因为大 value 的复制帧走的是同一条总线（见上面的注释）。所以期限里加上
    /// "按声明长度折算的时间"，下限速率取 1MB/s——任何真实链路都比它快，而"每 10 秒
    /// 滴 1 字节"要凑完 256MB 需要几十年，一定超。
    static constexpr uint64_t kPartialFrameGraceMs = 10000;
    static constexpr uint64_t kPartialFrameMinBytesPerSec = 1024ull * 1024ull;

    /// @brief 当前这一帧第一次残缺的时刻（steady_clock 毫秒），0 表示没有半帧挂着。
    /// 只在换帧时重置，按进展重置会被滴包永远续下去。
    uint64_t partial_frame_since_ms_ = 0;

    /// @brief 上次记账时缓冲里已有多少字节，用来区分"换了一帧"与"同一帧还在长"。
    size_t partial_frame_bytes_ = 0;

    /// @brief 上次记账时那一帧声明的长度；变了就说明是另一条帧，重新起算。
    uint32_t partial_frame_declared_ = 0;

    /// @brief 清掉半帧记账（帧凑齐、帧换了、或没有半帧挂着时）。
    void reset_partial_frame_accounting();

    /// @brief 这条链路当初登记进 EventLoop 的 fd；断开后 fd_ 变 -1，它仍然有效。
    int registered_fd_ = -1;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_LINK_H