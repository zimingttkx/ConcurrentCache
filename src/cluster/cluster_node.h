#ifndef CONCURRENTCACHE_CLUSTER_NODE_H
#define CONCURRENTCACHE_CLUSTER_NODE_H

#include "cluster_common.h"
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>

namespace cc_server {

class ClusterNode {
public:
    // 构造函数
    ClusterNode(const std::string& name, const std::string& ip, int port, NodeRole role);

    // 获取节点信息（[[nodiscard]] 防止忽略返回值）
    [[nodiscard]] const NodeInfo& getInfo() const { return info_; }
    [[nodiscard]] NodeInfo& getInfo() { return info_; }

    // 节点名称
    [[nodiscard]] const std::string& getName() const { return info_.name; }

    // 角色相关
    [[nodiscard]] NodeRole getRole() const { return info_.role; }
    void setRole(NodeRole role) { info_.role = role; }
    [[nodiscard]] bool isMaster() const { return info_.role == NodeRole::kMaster; }
    [[nodiscard]] bool isReplica() const { return info_.role == NodeRole::kReplica; }

    // 标志相关
    [[nodiscard]] uint64_t getFlags() const { return info_.flags; }
    void setFlags(uint64_t flags) { info_.flags = flags; }
    void addFlags(uint64_t flags) { info_.flags |= flags; }
    void clearFlags(uint64_t flags) { info_.flags &= ~flags; }
    [[nodiscard]] bool hasFlags(uint64_t flags) const { return (info_.flags & flags) != 0; }

    // 连接状态
    void setConnected(bool connected);
    [[nodiscard]] bool isConnected() const;

    // 心跳相关
    void updatePingSent(int64_t time) { info_.ping_sent = time; }
    void updatePongReceived(int64_t time) { info_.pong_received = time; }
    [[nodiscard]] int64_t getPingSent() const { return info_.ping_sent; }
    [[nodiscard]] int64_t getPongReceived() const { return info_.pong_received; }

    // 槽相关
    void addSlot(int slot);
    void delSlot(int slot);
    [[nodiscard]] bool hasSlot(int slot) const;
    [[nodiscard]] const std::vector<int>& getSlots() const { return slots_; }

    // 主从关系（副本用）
    void setMaster(const std::string& ip, int port);
    [[nodiscard]] bool hasMaster() const { return !info_.replicaof_ip.empty(); }

    // 复制相关
    void setReplicationState(ReplicationState state) { replication_state_ = state; }
    [[nodiscard]] ReplicationState getReplicationState() const { return replication_state_; }
    void setMasterNode(std::shared_ptr<ClusterNode> node) { master_node_ = node; }
    [[nodiscard]] std::shared_ptr<ClusterNode> getMasterNode() const { return master_node_.lock(); }
    void setMasterReplOffset(int64_t offset) { master_repl_offset_ = offset; }
    [[nodiscard]] int64_t getMasterReplOffset() const { return master_repl_offset_; }

    // 故障检测相关
    void setFailFlag(bool fail);                                              // 设置/清除 FAIL 标志
    void setPfailFlag(bool pfail);                                           // 设置/清除 PFAIL 标志
    [[nodiscard]] bool isFailed() const { return hasFlags(static_cast<uint64_t>(NodeFlags::kFail)); }  // 是否客观下线
    [[nodiscard]] bool isPfailed() const { return hasFlags(static_cast<uint64_t>(NodeFlags::kPfail)); }  // 是否疑似下线
    void incrementFailureCount() { failure_count_++; }                        // 增加失败计数
    void resetFailureCount() { failure_count_ = 0; }                        // 重置失败计数
    [[nodiscard]] int getFailureCount() const { return failure_count_; }    // 获取失败计数
    void setFirstPfailTime(int64_t time) { first_pfail_time_ = time; }    // 设置首次疑似下线时间
    [[nodiscard]] int64_t getFirstPfailTime() const { return first_pfail_time_; }  // 获取首次疑似下线时间
    void setFailTime(int64_t time) { fail_time_ = time; }                 // 设置客观下线时间
    [[nodiscard]] int64_t getFailTime() const { return fail_time_; }       // 获取客观下线时间

    // 故障转移相关
    void setFailoverState(FailoverState state) { failover_state_ = state; }  // 设置故障转移状态
    [[nodiscard]] FailoverState getFailoverState() const { return failover_state_; }  // 获取故障转移状态
    void setFailoverEpoch(int64_t epoch) { failover_epoch_ = epoch; }  // 设置故障转移轮次
    [[nodiscard]] int64_t getFailoverEpoch() const { return failover_epoch_; }  // 获取故障转移轮次
    void addVote(const std::string& node_name, int64_t epoch, int64_t offset);  // 添加投票
    [[nodiscard]] int getVoteCount() const { return static_cast<int>(votes_.size()); }  // 获取票数
    [[nodiscard]] bool hasVoted() const { return !votes_.empty(); }  // 是否已投票
    void clearVotes() { votes_.clear(); }  // 清除投票
    [[nodiscard]] int64_t getMaxVotedOffset() const;  // 获取最大投票偏移量
    void setReplicaPriority(int priority) { replica_priority_ = priority; }  // 设置从节点优先级
    [[nodiscard]] int getReplicaPriority() const { return replica_priority_; }  // 获取从节点优先级
    void setFailoverStartTime(int64_t time) { failover_start_time_ = time; }  // 设置故障转移开始时间
    [[nodiscard]] int64_t getFailoverStartTime() const { return failover_start_time_; }  // 获取故障转移开始时间

private:
    NodeInfo info_;                      // 节点信息
    std::vector<int> slots_;              // 该节点负责的槽
    std::atomic<bool> connected_{false};  // 连接状态
    mutable std::mutex mutex_;             // 保护 slots_

    // 复制相关
    ReplicationState replication_state_ = ReplicationState::kNone;  // 复制状态
    std::weak_ptr<ClusterNode> master_node_;                       // 主节点指针
    std::atomic<int64_t> master_repl_offset_{0};                  // 主节点复制偏移量

    // 故障检测相关
    int failure_count_ = 0;               // PING 超时次数
    int64_t first_pfail_time_ = 0;      // 首次疑似下线时间
    int64_t fail_time_ = 0;             // 客观下线确认时间

    // 故障转移相关
    FailoverState failover_state_ = FailoverState::kNoFailover;  // 故障转移状态
    int64_t failover_epoch_ = 0;         // 故障转移轮次
    std::vector<VoteInfo> votes_;        // 收集的投票
    int replica_priority_ = 100;         // 从节点优先级
    int64_t failover_start_time_ = 0;    // 故障转移开始时间（毫秒）
};

} // namespace cc_server

#endif
