// cluster_gossip.cpp
#include "cluster_gossip.h"
#include "cluster_state.h"
#include "base/log.h"
#include <algorithm>
#include <cassert>
#include <random>

namespace cc_server {

ClusterGossip::ClusterGossip() {
    LOG_INFO(CLUSTER, "ClusterGossip created");
}

void ClusterGossip::init(ClusterState* state) {
    state_ = state;
    LOG_INFO(CLUSTER, "ClusterGossip initialized");
}

GossipMsg ClusterGossip::build_ping_msg() {
    GossipMsg msg;
    msg.type = GossipType::kPing;
    msg.sender_epoch = 0;

    if (!state_) {
        return msg;
    }

    auto my_node = state_->getNode(state_->getMyNodeName());
    if (!my_node) {
        return msg;
    }

    msg.sender_name = my_node->getName();

    // 携带本节点信息
    GossipNodeInfo info;
    info.name = my_node->getName();
    info.ip = my_node->getInfo().ip;
    info.port = my_node->getInfo().port;
    info.flags = my_node->getFlags();
    info.role = my_node->isMaster() ? 0 : 1;
    info.epoch = msg.sender_epoch;

    const auto& slots = my_node->getSlots();
    info.slot_count = static_cast<uint16_t>(std::min(slots.size(), static_cast<size_t>(16384)));
    for (size_t i = 0; i < info.slot_count; ++i) {
        info.used_slot.push_back(static_cast<uint16_t>(slots[i]));
    }

    msg.nodes.push_back(info);

    return msg;
}

GossipMsg ClusterGossip::build_pong_msg() {
    GossipMsg msg;
    msg.type = GossipType::kPong;
    msg.sender_epoch = 0;

    if (!state_) {
        return msg;
    }

    auto my_node = state_->getNode(state_->getMyNodeName());
    if (!my_node) {
        return msg;
    }

    msg.sender_name = my_node->getName();

    // 携带更多节点信息作为响应
    auto all_nodes = state_->getAllNodes();
    for (const auto& node : all_nodes) {
        GossipNodeInfo info;
        info.name = node->getName();
        info.ip = node->getInfo().ip;
        info.port = node->getInfo().port;
        info.flags = node->getFlags();
        info.role = node->isMaster() ? 0 : 1;

        const auto& slots = node->getSlots();
        info.slot_count = static_cast<uint16_t>(std::min(slots.size(), static_cast<size_t>(16384)));
        for (size_t i = 0; i < info.slot_count; ++i) {
            info.used_slot.push_back(static_cast<uint16_t>(slots[i]));
        }

        msg.nodes.push_back(info);
    }

    return msg;
}

GossipMsg ClusterGossip::build_meet_msg(const std::string& ip, int port) {
    (void)ip;
    (void)port;
    GossipMsg msg;
    msg.type = GossipType::kMeet;
    msg.sender_epoch = 0;

    if (!state_) {
        return msg;
    }

    auto my_node = state_->getNode(state_->getMyNodeName());
    if (!my_node) {
        return msg;
    }

    msg.sender_name = my_node->getName();

    // MEET 消息包含本节点信息
    GossipNodeInfo info;
    info.name = my_node->getName();
    info.ip = my_node->getInfo().ip;
    info.port = my_node->getInfo().port;
    info.flags = my_node->getFlags();
    info.role = my_node->isMaster() ? 0 : 1;

    msg.nodes.push_back(info);

    return msg;
}

void ClusterGossip::handle_ping(const GossipMsg& msg) {
    assert(state_ != nullptr);
    LOG_DEBUG(CLUSTER, "Received PING from %s (epoch=%lu)",
              msg.sender_name.c_str(), msg.sender_epoch);

    // 处理消息中的节点信息
    for (const auto& info : msg.nodes) {
        // 更新已知节点信息
        known_nodes_[info.name] = info;

        // 如果是未知节点，触发 meet 回调
        auto existing = state_->getNode(info.name);
        if (!existing) {
            LOG_INFO(CLUSTER, "Learned about new node via gossip: %s (%s:%d)",
                     info.name.c_str(), info.ip.c_str(), info.port);

            if (meet_callback_) {
                meet_callback_(info.ip, info.port);
            }
        } else {
            // 更新已有节点信息
            if (update_callback_) {
                update_callback_(existing);
            }
        }
    }
}

void ClusterGossip::handle_pong(const GossipMsg& msg) {
    assert(state_ != nullptr);
    LOG_DEBUG(CLUSTER, "Received PONG from %s", msg.sender_name.c_str());

    // PONG 消息主要用来更新时间戳
    // 也携带节点信息用于同步
    for (const auto& info : msg.nodes) {
        known_nodes_[info.name] = info;

        auto existing = state_->getNode(info.name);
        if (existing && update_callback_) {
            update_callback_(existing);
        }
    }
}

void ClusterGossip::handle_meet(const GossipMsg& msg) {
    assert(state_ != nullptr);
    LOG_INFO(CLUSTER, "Received MEET from %s", msg.sender_name.c_str());

    for (const auto& info : msg.nodes) {
        // 添加或更新节点
        auto existing = state_->getNode(info.name);
        if (!existing) {
            auto node = std::make_shared<ClusterNode>(
                info.name, info.ip, info.port,
                info.role == 0 ? NodeRole::kMaster : NodeRole::kReplica
            );
            node->setFlags(info.flags);
            state_->addNode(node);

            LOG_INFO(CLUSTER, "Added node via MEET: %s (%s:%d)",
                     info.name.c_str(), info.ip.c_str(), info.port);
        }
    }

    // 触发 meet 回调
    if (meet_callback_) {
        // 从 sender_name 获取 IP:port
        auto it = known_nodes_.find(msg.sender_name);
        if (it != known_nodes_.end()) {
            meet_callback_(it->second.ip, it->second.port);
        }
    }
}

void ClusterGossip::send_gossip(const GossipMsg& msg) {
    (void)msg;
    // 实际发送由 ClusterConnection 处理
    // 这里只是构建消息
}

std::vector<std::shared_ptr<ClusterNode>> ClusterGossip::get_random_nodes(size_t count) {
    assert(state_ != nullptr);
    auto all_nodes = state_->getAllNodes();

    if (all_nodes.empty() || count >= all_nodes.size()) {
        return all_nodes;
    }

    // 随机选择 count 个节点
    std::vector<std::shared_ptr<ClusterNode>> result;
    std::sample(
        all_nodes.begin(),
        all_nodes.end(),
        std::back_inserter(result),
        count,
        std::mt19937{std::random_device{}()}
    );

    return result;
}

void ClusterGossip::push_node_info(const std::shared_ptr<ClusterNode>& node) {
    GossipNodeInfo info;
    info.name = node->getName();
    info.ip = node->getInfo().ip;
    info.port = node->getInfo().port;
    info.flags = node->getFlags();
    info.role = node->isMaster() ? 0 : 1;

    const auto& slots = node->getSlots();
    info.slot_count = static_cast<uint16_t>(std::min(slots.size(), static_cast<size_t>(16384)));
    for (size_t i = 0; i < info.slot_count; ++i) {
        info.used_slot.push_back(static_cast<uint16_t>(slots[i]));
    }

    known_nodes_[info.name] = info;
}

void ClusterGossip::pull_node_info() {
    // 向邻居节点请求最新信息
    // 实际请求由 ClusterConnection 处理
}

void ClusterGossip::handle_fail(const GossipMsg& msg) {
    assert(state_ != nullptr);
    LOG_INFO(CLUSTER, "Received FAIL broadcast from %s", msg.sender_name.c_str());

    // 处理 FAIL 消息中的节点信息
    for (const auto& info : msg.nodes) {
        auto node = state_->getNode(info.name);
        if (node) {
            // 标记节点为 FAIL
            node->setFailFlag(true);
            LOG_INFO(CLUSTER, "Node %s marked as FAIL via gossip from %s",
                     info.name.c_str(), msg.sender_name.c_str());
        }
    }
}

void ClusterGossip::broadcast_fail(const std::string& node_name) {
    if (!state_) return;

    auto my_node = state_->getNode(state_->getMyNodeName());
    if (!my_node) return;

    GossipMsg msg;
    msg.type = GossipType::kFail;
    msg.sender_name = my_node->getName();
    msg.sender_epoch = my_node->getInfo().config_epoch;

    // 添加要广播的故障节点信息
    auto failed_node = state_->getNode(node_name);
    if (failed_node) {
        GossipNodeInfo info;
        info.name = failed_node->getName();
        info.ip = failed_node->getInfo().ip;
        info.port = failed_node->getInfo().port;
        info.flags = failed_node->getFlags();
        info.role = failed_node->isMaster() ? 0 : 1;
        msg.nodes.push_back(info);

        LOG_INFO(CLUSTER, "Broadcasting FAIL for node %s", node_name.c_str());
    }

    // 实际发送由 ClusterConnection 处理
    // 这里只是将消息放入发送队列
    send_gossip(msg);
}

} // namespace cc_server