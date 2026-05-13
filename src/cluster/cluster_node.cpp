// cluster_node.cpp
#include "cluster_node.h"
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
        fail_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        LOG_INFO(CLUSTER, "Node %s marked as FAIL (客观下线)", info_.name.c_str());
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kFail));
        fail_time_ = 0;
    }
}

void ClusterNode::setPfailFlag(bool pfail) {
    if (pfail) {
        addFlags(static_cast<uint64_t>(NodeFlags::kPfail));
        if (first_pfail_time_ == 0) {
            first_pfail_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        LOG_INFO(CLUSTER, "Node %s marked as PFAIL (疑似下线)", info_.name.c_str());
    } else {
        clearFlags(static_cast<uint64_t>(NodeFlags::kPfail));
        first_pfail_time_ = 0;
        failure_count_ = 0;
    }
}

void ClusterNode::addVote(const std::string& node_name, int64_t epoch, int64_t offset) {
    VoteInfo vote;
    vote.node_name = node_name;
    vote.epoch = epoch;
    vote.offset = offset;
    votes_.push_back(vote);
    LOG_INFO(CLUSTER, "Node %s voted for %s (epoch=%ld, offset=%ld)",
             info_.name.c_str(), node_name.c_str(), epoch, offset);
}

int64_t ClusterNode::getMaxVotedOffset() const {
    int64_t max_offset = 0;
    for (const auto& vote : votes_) {
        if (vote.offset > max_offset) {
            max_offset = vote.offset;
        }
    }
    return max_offset;
}

} // namespace cc_server
