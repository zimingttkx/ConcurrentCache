// cluster_node.cpp
#include "cluster_node.h"
#include "cluster_gossip.h"  // 提供 GossipNodeInfo 完整定义（updateNodeInfo 使用）
#include "cluster_common.h"
#include "base/log.h"
#include <algorithm>

namespace cc_server {

ClusterNode::ClusterNode(const std::string& name, const std::string& ip, int port, NodeRole role)
    : info_() {
    info_.name = name;
    info_.ip = ip;
    info_.port = port;
    info_.role = role;
    LOG_INFO(CLUSTER, "Created cluster node: name=%s, ip=%s, port=%d, role=%d",
             name.c_str(), ip.c_str(), port, static_cast<int>(role));
}

void ClusterNode::setConnected(bool connected) {
    connected_ = connected;
    LOG_DEBUG(CLUSTER, "Node %s connection status: %s",
             info_.name.c_str(), connected ? "connected" : "disconnected");
}

bool ClusterNode::isConnected() const {
    return connected_.load();
}

void ClusterNode::addSlot(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(slots_.begin(), slots_.end(), slot) == slots_.end()) {
        slots_.push_back(slot);
    }
}

void ClusterNode::delSlot(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase(slots_, slot);
}

void ClusterNode::updateNodeInfo(const GossipNodeInfo& info) {
    // 同步对端广播的节点元信息（flags/role/slots/config_epoch）。
    // 注意：这里只同步可安全合并的字段，避免覆盖本节点的本地状态
    // （如 connected_、replication_state_、投票记录等）。
    if (info.flags & static_cast<uint16_t>(NodeFlags::kFail)) {
        setFailFlag(true);
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kFail));
    }
    if (info.flags & static_cast<uint16_t>(NodeFlags::kPfail)) {
        setPfailFlag(true);
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kPfail));
    }
    if (info.role == 0) {
        setRole(NodeRole::kMaster);
    } else {
        setRole(NodeRole::kReplica);
    }
    // 修复 P0-4/High：config_epoch 现在为原子成员，使用 store 而非平铺赋值
    info_.config_epoch.store(static_cast<int64_t>(info.epoch), std::memory_order_relaxed);

    // 同步槽信息：用广播中的槽列表整体替换本地列表（仅在非故障转移期间安全）
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.assign(info.used_slot.begin(), info.used_slot.end());
    }
    LOG_DEBUG(CLUSTER, "Updated node %s info via gossip (flags=0x%X, role=%d, slots=%zu)",
              info_.name.c_str(), info.flags, static_cast<int>(info.role), info.used_slot.size());
}

bool ClusterNode::hasSlot(int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(slots_.begin(), slots_.end(), slot) != slots_.end();
}

void ClusterNode::setMaster(const std::string& ip, int port) {
    info_.replicaof_ip = ip;
    info_.replicaof_port = port;
    info_.role = NodeRole::kReplica;
}

void ClusterNode::setFailFlag(bool fail) {
    if (fail) {
        addFlags(static_cast<uint64_t>(NodeFlags::kFail));
        fail_time_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        LOG_INFO(CLUSTER, "Node %s marked as FAIL (客观下线)", info_.name.c_str());
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kFail));
        fail_time_.store(0, std::memory_order_relaxed);
    }
}

void ClusterNode::setPfailFlag(bool pfail) {
    if (pfail) {
        addFlags(static_cast<uint64_t>(NodeFlags::kPfail));
        if (first_pfail_time_.load(std::memory_order_relaxed) == 0) {
            first_pfail_time_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        }
        LOG_INFO(CLUSTER, "Node %s marked as PFAIL (疑似下线)", info_.name.c_str());
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kPfail));
        first_pfail_time_.store(0, std::memory_order_relaxed);
        failure_count_.store(0, std::memory_order_relaxed);
    }
}

void ClusterNode::addVote(const std::string& node_name, int64_t epoch, int64_t offset) {
    std::lock_guard<std::mutex> lock(votes_mutex_);
    VoteInfo vote;
    vote.node_name = node_name;
    vote.epoch = epoch;
    vote.offset = offset;
    votes_.push_back(vote);
    LOG_INFO(CLUSTER, "Node %s voted for %s (epoch=%ld, offset=%ld)",
             info_.name.c_str(), node_name.c_str(), epoch, offset);
}

int64_t ClusterNode::getMaxVotedOffset() const {
    std::lock_guard<std::mutex> lock(votes_mutex_);
    int64_t max_offset = 0;
    for (const auto& vote : votes_) {
        if (vote.offset > max_offset) {
            max_offset = vote.offset;
        }
    }
    return max_offset;
}

} // namespace cc_server
