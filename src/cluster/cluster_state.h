// cluster_state.h
#ifndef CONCURRENTCACHE_CLUSTER_STATE_H
#define CONCURRENTCACHE_CLUSTER_STATE_H

#include "cluster_node.h"
#include <cstddef>
#include <unordered_map>
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

    // 集群信息
    [[nodiscard]] std::string getMyNodeName() const { return my_node_name_; }
    void setMyNodeName(const std::string& name) { my_node_name_ = name; }

private:
    std::string my_node_name_;                                               // 本节点名称
    std::unordered_map<std::string, std::shared_ptr<ClusterNode>> nodes_;    // 节点列表
    std::unordered_map<int, std::shared_ptr<ClusterNode>> slots_;            // 槽映射表（存指针避免二次查找）
    mutable std::shared_mutex mutex_;                                        // 保护 nodes_
    mutable std::shared_mutex slots_mutex_;                                  // 保护 slots_
};

} // namespace cc_server

#endif
