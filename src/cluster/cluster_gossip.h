// cluster_gossip.h
#ifndef CONCURRENTCACHE_CLUSTER_GOSSIP_H
#define CONCURRENTCACHE_CLUSTER_GOSSIP_H

#include "cluster_link.h"
#include "cluster_state.h"
#include "cluster_node.h"
#include <unordered_map>
#include <vector>
#include <random>
#include <functional>

namespace cc_server {

// Gossip 消息类型
enum class GossipType : uint8_t {
    kPing = 1,       // 心跳
    kPong = 2,       // 心跳响应
    kMeet = 3,       // 节点加入请求
    kFail = 4,       // 节点故障广播
    kPush = 5,       // 推送节点信息
    kPull = 6,       // 拉取节点信息
};

// Gossip 节点信息（在网络间传播的节点信息）
struct GossipNodeInfo {
    std::string name;      // 节点名称
    std::string ip;        // IP 地址
    uint16_t port;         // 端口
    uint16_t flags;        // 节点标志
    uint64_t epoch;        // 配置 epoch
    uint32_t state;        // 集群状态
    uint8_t role;          // 节点角色 (0=master, 1=replica)
    uint16_t slot_count;   // 槽数量
    std::vector<uint16_t> used_slot; // 槽列表（动态数组，避免32KB栈占用）

    GossipNodeInfo() : port(0), flags(0), epoch(0), state(0), role(0), slot_count(0) {}
};

// Gossip 消息
struct GossipMsg {
    GossipType type;
    uint64_t sender_epoch;
    std::string sender_name;
    std::vector<GossipNodeInfo> nodes;  // 携带的节点信息

    GossipMsg() : type(GossipType::kPing), sender_epoch(0) {}
};

// ClusterGossip 类：Gossip 协议实现
class ClusterGossip {
public:
    ClusterGossip();

    // 初始化
    void init(ClusterState* state);

    // 构建 Gossip 消息
    GossipMsg build_ping_msg();
    GossipMsg build_pong_msg();
    GossipMsg build_meet_msg(const std::string& ip, int port);

    // 处理收到的消息
    void handle_ping(const GossipMsg& msg);
    void handle_pong(const GossipMsg& msg);
    void handle_meet(const GossipMsg& msg);
    void handle_fail(const GossipMsg& msg);

    // 发送消息给邻居
    void send_gossip(const GossipMsg& msg);

    // 广播 FAIL 消息
    void broadcast_fail(const std::string& node_name);

    // 获取随机节点列表（用于 Gossip 传播）
    std::vector<std::shared_ptr<ClusterNode>> get_random_nodes(size_t count);

    // 配置传播
    void push_node_info(const std::shared_ptr<ClusterNode>& node);
    void pull_node_info();

    // 设置回调
    using MeetCallback = std::function<void(const std::string& ip, int port)>;
    using UpdateCallback = std::function<void(const std::shared_ptr<ClusterNode>& node)>;
    void set_meet_callback(MeetCallback cb) { meet_callback_ = std::move(cb); }
    void set_update_callback(UpdateCallback cb) { update_callback_ = std::move(cb); }

private:
    ClusterState* state_ = nullptr;
    MeetCallback meet_callback_;
    UpdateCallback update_callback_;

    // 已知节点信息（用于 Gossip）
    std::unordered_map<std::string, GossipNodeInfo> known_nodes_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_GOSSIP_H