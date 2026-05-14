// cluster_state.h
#ifndef CONCURRENTCACHE_CLUSTER_STATE_H
#define CONCURRENTCACHE_CLUSTER_STATE_H

#include "cluster_node.h"
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace cc_server {

class ClusterState {
public:
    ClusterState();

    // 节点管理
    void addNode(std::shared_ptr<ClusterNode> node);
    void delNode(const std::string& name);
    [[nodiscard]] std::shared_ptr<ClusterNode> getNode(const std::string& name) const;
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeByIpPort(const std::string& ip, int port) const;
    [[nodiscard]] std::vector<std::shared_ptr<ClusterNode>> getAllNodes() const;
    [[nodiscard]] size_t size() const;

    // 槽管理（存储 shared_ptr 避免二次查找）
    void setNodeForSlot(int slot, std::shared_ptr<ClusterNode> node);
    void delSlot(int slot);
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeForSlot(int slot) const;
    [[nodiscard]] int getSlotOwnerCount() const;

    // 槽迁移状态管理
    void setSlotMigrating(int slot, const std::string& target_node);   // 标记槽正在迁出
    void setSlotImporting(int slot, const std::string& source_node);    // 标记槽正在迁入
    void clearSlotMigration(int slot);                                  // 清除槽迁移状态
    [[nodiscard]] SlotMigrationInfo getSlotMigrationInfo(int slot) const;  // 获取槽迁移信息
    [[nodiscard]] bool isSlotMigrating(int slot) const;                // 槽是否正在迁出
    [[nodiscard]] bool isSlotImporting(int slot) const;                // 槽是否正在迁入
    [[nodiscard]] std::vector<int> getMigratingSlots() const;          // 获取所有正在迁出的槽
    [[nodiscard]] std::vector<int> getImportingSlots() const;          // 获取所有正在迁入的槽

    // 主从复制管理
    void addReplica(const std::string& master_name, std::shared_ptr<ClusterNode> replica);   // 添加从节点
    void removeReplica(const std::string& master_name, const std::string& replica_name);      // 移除从节点
    [[nodiscard]] std::vector<std::shared_ptr<ClusterNode>> getReplicas(const std::string& master_name) const;  // 获取主节点的所有从节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getMasterOfReplica(const std::string& replica_name) const;  // 获取从节点的主节点

    // 故障检测管理
    void markNodeAsPfail(const std::string& node_name);    // 标记节点为疑似下线
    void markNodeAsFail(const std::string& node_name);     // 标记节点为客观下线
    void clearNodePfail(const std::string& node_name);     // 清除疑似下线状态
    void clearNodeFail(const std::string& node_name);       // 清除客观下线状态
    [[nodiscard]] std::vector<std::shared_ptr<ClusterNode>> getFailedNodes() const;     // 获取所有客观下线的节点
    [[nodiscard]] std::vector<std::shared_ptr<ClusterNode>> getPfailedNodes() const;   // 获取所有疑似下线的节点

    // 集群信息
    [[nodiscard]] std::string getMyNodeName() const { return my_node_name_; }
    void setMyNodeName(const std::string& name) { my_node_name_ = name; }

    // PFAIL 报告收集（用于故障转移法定人数检查）
    void addPfailReport(const std::string& node_name, const std::string& reporter_name);
    [[nodiscard]] size_t getPfailReportCount(const std::string& node_name) const;
    void clearPfailReports(const std::string& node_name);

private:
    std::string my_node_name_;                                               // 本节点名称
    std::unordered_map<std::string, std::shared_ptr<ClusterNode>> nodes_;    // 节点列表
    std::unordered_map<int, std::shared_ptr<ClusterNode>> slots_;            // 槽映射表（存指针避免二次查找）
    std::unordered_map<int, SlotMigrationInfo> migrating_slots_;              // 正在迁出的槽 (slot -> info)
    std::unordered_map<int, SlotMigrationInfo> importing_slots_;               // 正在迁入的槽 (slot -> info)
    std::unordered_map<std::string, std::vector<std::shared_ptr<ClusterNode>>> replicas_;  // 主节点名称 -> 从节点列表
    std::unordered_map<std::string, std::unordered_set<std::string>> pfailing_reports_;  // PFAIL 报告: 被报告节点 -> 报告者集合
    mutable std::shared_mutex mutex_;                                        // 保护 nodes_
    mutable std::shared_mutex slots_mutex_;                                  // 保护 slots_
    mutable std::shared_mutex migration_mutex_;                              // 保护迁移状态
    mutable std::shared_mutex replicas_mutex_;                               // 保护 replicas_
    mutable std::shared_mutex pfail_mutex_;                                  // 保护 pfailing_reports_
};

} // namespace cc_server

#endif
