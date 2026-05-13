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

    // 清理 replicas_ 中该节点作为主节点的从节点列表
    {
        std::unique_lock<std::shared_mutex> lock(replicas_mutex_);
        replicas_.erase(name);
        LOG_DEBUG(CLUSTER, "Removed replica list for master: %s", name.c_str());
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

    // 使用 std::lock 同时获取两个锁，避免死锁
    // 锁顺序：slots_mutex_ -> migration_mutex_
    std::lock(slots_mutex_, migration_mutex_);
    std::unique_lock<std::shared_mutex> slots_lock(slots_mutex_, std::adopt_lock);
    std::unique_lock<std::shared_mutex> migration_lock(migration_mutex_, std::adopt_lock);

    slots_.erase(slot);
    migrating_slots_.erase(slot);
    importing_slots_.erase(slot);
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

void ClusterState::setSlotMigrating(int slot, const std::string& target_node) {
    assert(slot >= 0 && slot <= 16383 && "setSlotMigrating: slot out of range");
    assert(!target_node.empty() && "setSlotMigrating: target_node is empty");

    std::unique_lock<std::shared_mutex> lock(migration_mutex_);
    SlotMigrationInfo info;
    info.slot = slot;
    info.status = SlotMigrationStatus::kMigrating;
    info.target_node = target_node;
    migrating_slots_[slot] = info;
    LOG_INFO(CLUSTER, "Slot %d set to MIGRATING, target=%s", slot, target_node.c_str());
}

void ClusterState::setSlotImporting(int slot, const std::string& source_node) {
    assert(slot >= 0 && slot <= 16383 && "setSlotImporting: slot out of range");
    assert(!source_node.empty() && "setSlotImporting: source_node is empty");

    std::unique_lock<std::shared_mutex> lock(migration_mutex_);
    SlotMigrationInfo info;
    info.slot = slot;
    info.status = SlotMigrationStatus::kImporting;
    info.source_node = source_node;
    importing_slots_[slot] = info;
    LOG_INFO(CLUSTER, "Slot %d set to IMPORTING, source=%s", slot, source_node.c_str());
}

void ClusterState::clearSlotMigration(int slot) {
    assert(slot >= 0 && slot <= 16383 && "clearSlotMigration: slot out of range");

    std::unique_lock<std::shared_mutex> lock(migration_mutex_);
    migrating_slots_.erase(slot);
    importing_slots_.erase(slot);
    LOG_INFO(CLUSTER, "Slot %d migration state cleared", slot);
}

SlotMigrationInfo ClusterState::getSlotMigrationInfo(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "getSlotMigrationInfo: slot out of range");

    std::shared_lock<std::shared_mutex> lock(migration_mutex_);
    SlotMigrationInfo empty;

    auto it = migrating_slots_.find(slot);
    if (it != migrating_slots_.end()) {
        return it->second;
    }

    it = importing_slots_.find(slot);
    if (it != importing_slots_.end()) {
        return it->second;
    }

    return empty;
}

bool ClusterState::isSlotMigrating(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "isSlotMigrating: slot out of range");

    std::shared_lock<std::shared_mutex> lock(migration_mutex_);
    return migrating_slots_.find(slot) != migrating_slots_.end();
}

bool ClusterState::isSlotImporting(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "isSlotImporting: slot out of range");

    std::shared_lock<std::shared_mutex> lock(migration_mutex_);
    return importing_slots_.find(slot) != importing_slots_.end();
}

std::vector<int> ClusterState::getMigratingSlots() const {
    std::shared_lock<std::shared_mutex> lock(migration_mutex_);
    std::vector<int> result;
    result.reserve(migrating_slots_.size());
    for (const auto& [slot, _] : migrating_slots_) {
        result.push_back(slot);
    }
    return result;
}

std::vector<int> ClusterState::getImportingSlots() const {
    std::shared_lock<std::shared_mutex> lock(migration_mutex_);
    std::vector<int> result;
    result.reserve(importing_slots_.size());
    for (const auto& [slot, _] : importing_slots_) {
        result.push_back(slot);
    }
    return result;
}

void ClusterState::addReplica(const std::string& master_name, std::shared_ptr<ClusterNode> replica) {
    assert(!master_name.empty() && "addReplica: master_name is empty");
    assert(replica != nullptr && "addReplica: replica is null");

    std::unique_lock<std::shared_mutex> lock(replicas_mutex_);
    replicas_[master_name].push_back(replica);
    LOG_INFO(CLUSTER, "Added replica %s to master %s",
             replica->getName().c_str(), master_name.c_str());
}

void ClusterState::removeReplica(const std::string& master_name, const std::string& replica_name) {
    assert(!master_name.empty() && "removeReplica: master_name is empty");
    assert(!replica_name.empty() && "removeReplica: replica_name is empty");

    std::unique_lock<std::shared_mutex> lock(replicas_mutex_);
    auto it = replicas_.find(master_name);
    if (it == replicas_.end()) {
        return;
    }

    auto& replica_list = it->second;
    for (auto it2 = replica_list.begin(); it2 != replica_list.end(); ++it2) {
        if ((*it2)->getName() == replica_name) {
            replica_list.erase(it2);
            LOG_INFO(CLUSTER, "Removed replica %s from master %s",
                     replica_name.c_str(), master_name.c_str());
            break;
        }
    }

    // 如果主节点没有从节点了，删除条目
    if (replica_list.empty()) {
        replicas_.erase(it);
    }
}

std::vector<std::shared_ptr<ClusterNode>> ClusterState::getReplicas(const std::string& master_name) const {
    assert(!master_name.empty() && "getReplicas: master_name is empty");

    std::shared_lock<std::shared_mutex> lock(replicas_mutex_);
    std::vector<std::shared_ptr<ClusterNode>> result;

    auto it = replicas_.find(master_name);
    if (it != replicas_.end()) {
        result = it->second;
    }
    return result;
}

std::shared_ptr<ClusterNode> ClusterState::getMasterOfReplica(const std::string& replica_name) const {
    assert(!replica_name.empty() && "getMasterOfReplica: replica_name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(replica_name);
    if (it != nodes_.end()) {
        return it->second->getMasterNode();
    }
    return nullptr;
}

void ClusterState::markNodeAsPfail(const std::string& node_name) {
    assert(!node_name.empty() && "markNodeAsPfail: node_name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(node_name);
    if (it != nodes_.end()) {
        it->second->setPfailFlag(true);
        it->second->incrementFailureCount();
        LOG_INFO(CLUSTER, "Node %s marked as PFAIL", node_name.c_str());
    }
}

void ClusterState::markNodeAsFail(const std::string& node_name) {
    assert(!node_name.empty() && "markNodeAsFail: node_name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(node_name);
    if (it != nodes_.end()) {
        it->second->setFailFlag(true);
        LOG_INFO(CLUSTER, "Node %s marked as FAIL", node_name.c_str());
    }
}

void ClusterState::clearNodePfail(const std::string& node_name) {
    assert(!node_name.empty() && "clearNodePfail: node_name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(node_name);
    if (it != nodes_.end()) {
        it->second->setPfailFlag(false);
        LOG_INFO(CLUSTER, "Node %s PFAIL cleared", node_name.c_str());
    }
}

void ClusterState::clearNodeFail(const std::string& node_name) {
    assert(!node_name.empty() && "clearNodeFail: node_name is empty");

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = nodes_.find(node_name);
    if (it != nodes_.end()) {
        it->second->setFailFlag(false);
        LOG_INFO(CLUSTER, "Node %s FAIL cleared", node_name.c_str());
    }
}

std::vector<std::shared_ptr<ClusterNode>> ClusterState::getFailedNodes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClusterNode>> result;
    for (const auto& [name, node] : nodes_) {
        if (node->isFailed()) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<std::shared_ptr<ClusterNode>> ClusterState::getPfailedNodes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClusterNode>> result;
    for (const auto& [name, node] : nodes_) {
        if (node->isPfailed()) {
            result.push_back(node);
        }
    }
    return result;
}

} // namespace cc_server
