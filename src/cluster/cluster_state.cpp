// cluster_state.cpp
#include "cluster_state.h"
#include "base/log.h"
#include <algorithm>
#include <cassert>

namespace cc_server {

ClusterState::ClusterState() {
    LOG_INFO(CLUSTER, "ClusterState initialized");
}

void ClusterState::addNode(std::shared_ptr<ClusterNode> node) {
    // 防御性检查：node 不能为空
    assert(node != nullptr && "addNode: node is null");
    if (!node) {
        LOG_ERROR(CLUSTER, "Attempted to add null node");
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // 检查节点是否已存在
    const std::string& name = node->getName();
    if (nodes_.find(name) != nodes_.end()) {
        LOG_WARN(CLUSTER, "Node already exists: name=%s", name.c_str());
        return;
    }

    nodes_[name] = node;
    LOG_INFO(CLUSTER, "Added node to cluster: name=%s", name.c_str());
}

void ClusterState::delNode(const std::string& name) {
    // 防御性检查：name 不能为空
    assert(!name.empty() && "delNode: name is empty");

    // 先获取节点的槽信息（需要先锁 mutex_ 再锁 slots_mutex_）
    std::shared_ptr<ClusterNode> node_to_delete;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = nodes_.find(name);
        if (it == nodes_.end()) {
            LOG_WARN(CLUSTER, "Node not found for deletion: name=%s", name.c_str());
            return;
        }
        node_to_delete = it->second;
    }

    // 获取该节点负责的所有槽
    std::vector<int> slots_to_remove = node_to_delete->getSlots();

    // 清理 slots_ 中指向该节点的引用
    {
        std::unique_lock<std::shared_mutex> lock(slots_mutex_);
        for (int slot : slots_to_remove) {
            auto it = slots_.find(slot);
            if (it != slots_.end() && it->second == node_to_delete) {
                slots_.erase(it);
            }
        }
    }

    // 删除节点
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        nodes_.erase(name);
        LOG_INFO(CLUSTER, "Removed node from cluster: name=%s", name.c_str());
    }
}

std::shared_ptr<ClusterNode> ClusterState::getNode(const std::string& name) const {
    // 防御性检查：name 不能为空
    assert(!name.empty() && "getNode: name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(name);
    if (it != nodes_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<ClusterNode> ClusterState::getNodeByIpPort(const std::string& ip, int port) const {
    // 防御性检查：ip 不能为空，port 必须 > 0
    assert(!ip.empty() && "getNodeByIpPort: ip is empty");
    assert(port > 0 && "getNodeByIpPort: port must be positive");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [name, node] : nodes_) {
        if (node->getInfo().ip == ip && node->getInfo().port == port) {
            return node;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<ClusterNode>> ClusterState::getAllNodes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClusterNode>> result;
    result.reserve(nodes_.size());
    for (const auto& [name, node] : nodes_) {
        result.push_back(node);
    }
    return result;
}

size_t ClusterState::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return nodes_.size();
}

void ClusterState::setNodeForSlot(int slot, std::shared_ptr<ClusterNode> node) {
    // 防御性检查：slot 必须在有效范围 [0, 16383]
    assert(slot >= 0 && slot <= 16383 && "setNodeForSlot: slot out of range");
    // 防御性检查：node 不能为空
    assert(node != nullptr && "setNodeForSlot: node is null");
    if (!node) {
        LOG_ERROR(CLUSTER, "Attempted to set null node for slot");
        return;
    }

    std::unique_lock<std::shared_mutex> lock(slots_mutex_);
    slots_[slot] = node;
}

void ClusterState::delSlot(int slot) {
    // 防御性检查：slot 必须在有效范围 [0, 16383]
    assert(slot >= 0 && slot <= 16383 && "delSlot: slot out of range");

    std::unique_lock<std::shared_mutex> lock(slots_mutex_);
    slots_.erase(slot);
}

std::shared_ptr<ClusterNode> ClusterState::getNodeForSlot(int slot) const {
    // 防御性检查：slot 必须在有效范围 [0, 16383]
    assert(slot >= 0 && slot <= 16383 && "getNodeForSlot: slot out of range");

    std::shared_lock<std::shared_mutex> lock(slots_mutex_);
    auto it = slots_.find(slot);
    if (it != slots_.end()) {
        return it->second;
    }
    return nullptr;
}

int ClusterState::getSlotOwnerCount() const {
    std::shared_lock<std::shared_mutex> lock(slots_mutex_);
    return static_cast<int>(slots_.size());
}

} // namespace cc_server
