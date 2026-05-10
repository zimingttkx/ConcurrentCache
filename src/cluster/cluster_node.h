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

private:
    NodeInfo info_;                      // 节点信息
    std::vector<int> slots_;              // 该节点负责的槽
    std::atomic<bool> connected_{false};  // 连接状态
    mutable std::mutex mutex_;             // 保护 slots_
};

} // namespace cc_server

#endif
