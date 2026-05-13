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
};

// 集群消息头
struct ClusterMsgHeader {
    uint32_t magic;           // 消息标识 (0x43 = 'C')
    uint16_t version;         // 协议版本
    uint16_t type;            // 消息类型
    uint16_t length;          // 消息长度
    uint64_t sender_epoch;    // 发送者 epoch
    char sender_name[40];     // 发送者节点名
    uint16_t flags;           // 发送者标志
    uint16_t port;            // 发送者端口
    uint32_t state;           // 集群状态
    // 槽位图：每个 bit 表示一个槽是否属于发送者（预留字段，用于未来优化）
    // 当前使用 GossipNodeInfo::used_slot 动态传递槽信息
    uint8_t slot_map[16384 / 8]; // 槽位图 (2048 bytes)
};

// 集群消息
struct ClusterMsg {
    ClusterMsgHeader header;
    std::vector<std::string> args;  // 消息参数

    ClusterMsg() {
        memset(&header, 0, sizeof(header));
        header.magic = 0x43;  // 'C'
        header.version = 1;
    }
};

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
    [[nodiscard]] bool is_connected() const { return connected_.load(); }

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

    // 属性访问
    [[nodiscard]] const std::string& node_name() const { return node_name_; }
    [[nodiscard]] const std::string& ip() const { return ip_; }
    [[nodiscard]] int port() const { return port_; }
    [[nodiscard]] int fd() const { return fd_; }

    // 回调设置
    void set_msg_callback(MsgCallback cb) { msg_callback_ = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { disconnect_callback_ = std::move(cb); }

    // 最后通信时间（用于超时检测）
    void update_last_recv_time();
    [[nodiscard]] int64_t last_recv_time() const { return last_recv_time_.load(); }

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
    Buffer recv_buffer_;                  // 接收缓冲区

    MsgCallback msg_callback_;            // 消息回调
    DisconnectCallback disconnect_callback_;  // 断开回调

    static constexpr uint32_t kMsgMagic = 0x43;  // 'C'
    static constexpr size_t kHeaderSize = sizeof(ClusterMsgHeader);
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_LINK_H