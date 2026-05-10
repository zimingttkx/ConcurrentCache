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

} // namespace cc_server
