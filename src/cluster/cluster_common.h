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
    kFail = 1 << 0,           // 节点已下线（客观下线）
    kPfail = 1 << 1,          // 节点疑似下线（主观下线）
    kHandshake = 1 << 2,      // 节点正在握手
    kNoAddress = 1 << 3,       // 节点地址未知
};

// 复制状态
enum class ReplicationState {
    kNone = 0,                // 无复制关系
    kConnect = 1,             // 正在连接主节点
    kHandshake = 2,           // 正在握手
    kSync = 3,                // 正在同步
    kConnected = 4,           // 复制已建立
};

// 槽迁移状态
enum class SlotMigrationStatus {
    kNone = 0,           // 槽无迁移状态
    kMigrating = 1,     // 源节点正在迁出
    kImporting = 2,      // 目标节点正在迁入
};

// 槽迁移信息结构体
struct SlotMigrationInfo {
    int slot = -1;                          // 槽号
    SlotMigrationStatus status = SlotMigrationStatus::kNone;  // 迁移状态
    std::string source_node;                 // 源节点名称 (迁出节点)
    std::string target_node;                 // 目标节点名称 (迁入节点)
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
    int64_t config_epoch = 0;     // 配置轮次（用于故障转移）
};

// 节点故障检测信息
struct NodeFailureInfo {
    std::string node_name;              // 节点名称
    int64_t first_pfail_time = 0;      // 首次疑似下线时间
    int64_t fail_time = 0;             // 确认下线时间（客观下线）
    int failure_count = 0;              // PING 超时次数
};

// 故障检测配置
struct FailureDetectionConfig {
    int64_t node_timeout_ms = 5000;    // 节点超时时间（毫秒）
    int max_ping_failures = 3;         // 超过此次数认为疑似下线
    int quorum = 2;                    // 客观下线需要的投票数
};

// 故障转移状态
enum class FailoverState {
    kNoFailover = 0,           // 未执行故障转移
    kWaitStart = 1,            // 等待开始（从节点检测到主节点FAIL）
    kWaitAuth = 2,             // 等待投票（等待主节点投票）
    kFailoverInProgress = 3,  // 故障转移进行中
    kFailoverCompleted = 4,    // 故障转移完成
    kFailoverTimeout = 5,      // 故障转移超时
};

// 故障转移配置
struct FailoverConfig {
    int64_t failover_timeout_ms = 30000;    // 故障转移超时时间（毫秒）
    int64_t auth_timeout_ms = 5000;        // 投票超时时间（毫秒）
    int replica_priority = 100;              // 从节点优先级
};

// 选举投票信息
struct VoteInfo {
    std::string node_name;          // 投票节点名称
    int64_t epoch = 0;              // 投票的epoch
    int64_t offset = 0;             // 投票节点的复制偏移量
};

} // namespace cc_server

#endif
