#ifndef CONCURRENTCACHE_CLUSTER_COMMON_H
#define CONCURRENTCACHE_CLUSTER_COMMON_H

#include <cstdint>
#include <string>

namespace cc_server {

// 节点角色
enum class NodeRole {
    kMaster = 0,      // 主节点
    kReplica = 1,     // 从节点
    kNodeUnknown = 2  // 未知/未定义
};

// 节点标志（可组合）
enum class NodeFlags {
    kNone = 0,
    kFail = 1 << 0,           // 节点已下线
    kPfail = 1 << 1,          // 节点疑似下线
    kHandshake = 1 << 2,      // 节点正在握手
    kNoAddress = 1 << 3,       // 节点地址未知
};

// 节点信息结构体
struct NodeInfo {
    std::string name;               // 节点唯一名称
    std::string ip;                // IP地址
    int port = 0;                  // 端口
    std::string replicaof_ip;       // 主节点IP（副本用）
    int replicaof_port = 0;         // 主节点端口（副本用）
    NodeRole role = NodeRole::kNodeUnknown;  // 角色
    uint64_t flags = 0;            // 标志
    int64_t ping_sent = 0;         // 发送ping的时间
    int64_t pong_received = 0;     // 收到pong的时间
    int64_t link_disconnect_time = 0;  // 连接断开时间
};

} // namespace cc_server

#endif
