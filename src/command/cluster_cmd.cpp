#include "cluster_cmd.h"
#include "cluster/cluster_server.h"
#include "cluster/cluster_node.h"
#include "cluster/cluster_connection.h"
#include "base/log.h"
#include "protocol/resp.h"
#include "cache/storage.h"
#include "datatype/object.h"
#include <cctype>
#include <cstdlib>

namespace cc_server {

std::string ClusterCommand::execute(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER <subcommand>
    if (args.size() < 2) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster' command");
    }

    const std::string& subcommand = args[1];

    // 子命令分发
    if (subcommand == "meet") {
        return handleMeet(args);
    } else if (subcommand == "nodes") {
        return handleNodes(args);
    } else if (subcommand == "info") {
        return handleInfo(args);
    } else if (subcommand == "addslots") {
        return handleAddSlots(args);
    } else if (subcommand == "slots") {
        return handleSlots(args);
    } else if (subcommand == "delslots") {
        return handleDelSlots(args);
    } else if (subcommand == "setslot") {
        return handleSetSlot(args);
    } else if (subcommand == "replicate") {
        return handleReplicate(args);
    } else if (subcommand == "fail") {
        return handleFail(args);
    } else if (subcommand == "migrate") {
        return handleMigrate(args);
    } else {
        return RespEncoder::encode_error("ERR Unknown CLUSTER subcommand");
    }
}

std::string ClusterCommand::handleMeet(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER MEET <ip> <port>
    if (args.size() < 4) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster meet' command");
    }

    const std::string& ip = args[2];

    // 验证 IP 地址格式
    if (!isValidIp(ip)) {
        return RespEncoder::encode_error("ERR invalid IP address");
    }

    // 解析端口号，带错误处理
    int port = 0;
    try {
        port = std::stoi(args[3]);
    } catch (...) {
        return RespEncoder::encode_error("ERR invalid port number");
    }

    if (port <= 0 || port > 65535) {
        return RespEncoder::encode_error("ERR invalid port number");
    }

    LOG_INFO(CLUSTER, "CLUSTER MEET request: ip=%s, port=%d", ip.c_str(), port);

    // 检查集群是否启用
    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    // 获取本节点
    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 检查是否是本节点
    if (my_node->getInfo().ip == ip && my_node->getInfo().port == port) {
        return RespEncoder::encode_error("ERR cannot meet yourself");
    }

    // 构建节点名称
    std::string name = ip + ":" + std::to_string(port);

    // 检查节点是否已存在
    ClusterState* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }
    auto existing = state->getNodeByIpPort(ip, port);
    if (existing) {
        // 节点已认识，检查是否需要建立连接
        auto* conn = ClusterServer::instance().getConnection();
        if (conn && !conn->is_node_connected(name)) {
            // 建立 TCP 连接
            conn->connect_to_node(name, ip, port);
            // 发送 MEET 消息
            conn->meet_node(name, my_node->getInfo().ip, my_node->getInfo().port);
        }
        return RespEncoder::encode_simple_string("OK");
    }

    // 创建握手节点并添加到状态
    auto node = std::make_shared<ClusterNode>(name, ip, port, NodeRole::kNodeUnknown);
    node->addFlags(static_cast<uint64_t>(NodeFlags::kHandshake));

    state->addNode(node);

    // 建立 TCP 连接并发送 MEET 消息
    auto* conn = ClusterServer::instance().getConnection();
    if (conn) {
        if (conn->connect_to_node(name, ip, port)) {
            conn->meet_node(name, my_node->getInfo().ip, my_node->getInfo().port);
            LOG_INFO(CLUSTER, "Established connection and sent MEET to %s:%d", ip.c_str(), port);
        } else {
            LOG_WARN(CLUSTER, "Failed to connect to %s:%d", ip.c_str(), port);
        }
    }

    LOG_INFO(CLUSTER, "Meet request sent to %s:%d", ip.c_str(), port);
    return RespEncoder::encode_simple_string("OK");
}

std::string ClusterCommand::handleNodes(const std::vector<std::string>& args) {
    (void)args;  // 未使用参数

    auto* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    auto nodes = state->getAllNodes();
    if (nodes.empty()) {
        return RespEncoder::encode_simple_string("no nodes");
    }

    std::string result;
    for (const auto& node : nodes) {
        const auto& info = node->getInfo();

        // 节点名称
        result += info.name;

        // IP:Port
        result += " " + info.ip + ":" + std::to_string(info.port);

        // 节点角色和标志
        if (node->isMaster()) {
            result += " master";
        } else if (node->isReplica()) {
            result += " slave";
        } else {
            result += " myself,?";
        }

        // 连接状态
        if (node->isConnected()) {
            result += " connected";
        } else {
            result += " disconnected";
        }

        // 槽信息
        const auto& slots = node->getSlots();
        if (slots.empty()) {
            result += " -";
        } else {
            result += " ";
            for (size_t i = 0; i < slots.size(); ++i) {
                result += std::to_string(slots[i]);
                if (i < slots.size() - 1) {
                    result += ",";
                }
            }
        }

        // 主节点信息（副本节点用）
        if (node->isReplica() && !info.replicaof_ip.empty()) {
            result += " " + info.replicaof_ip + ":" + std::to_string(info.replicaof_port);
        } else {
            result += " -";
        }

        result += "\n";
    }

    return RespEncoder::encode_bulk_string(result);
}

std::string ClusterCommand::handleInfo(const std::vector<std::string>& args) {
    (void)args;  // 未使用参数

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    auto nodes = state->getAllNodes();
    int master_count = 0;
    int replica_count = 0;
    int handshake_count = 0;
    int connected_count = 0;

    for (const auto& node : nodes) {
        if (node->hasFlags(static_cast<uint64_t>(NodeFlags::kHandshake))) {
            handshake_count++;
        } else if (node->isConnected()) {
            connected_count++;
        }
        if (node->isMaster()) {
            master_count++;
        } else if (node->isReplica()) {
            replica_count++;
        }
    }

    int slot_owner_count = state->getSlotOwnerCount();

    std::string result;
    result += "cluster_enabled:yes\n";
    result += "cluster_state:ok\n";
    result += "cluster_known_nodes:" + std::to_string(nodes.size()) + "\n";
    result += "cluster_master_nodes:" + std::to_string(master_count) + "\n";
    result += "cluster_replica_nodes:" + std::to_string(replica_count) + "\n";
    result += "cluster_handshake_nodes:" + std::to_string(handshake_count) + "\n";
    result += "cluster_connected_nodes:" + std::to_string(connected_count) + "\n";
    result += "cluster_slots_assigned:" + std::to_string(slot_owner_count) + "\n";
    result += "cluster_my_node:" + my_node->getName() + "\n";
    result += "cluster_current_epoch:" + std::to_string(0) + "\n";
    result += "cluster_stats_messages_received:" + std::to_string(0) + "\n";

    return RespEncoder::encode_bulk_string(result);
}

std::string ClusterCommand::handleAddSlots(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER ADDSLOTS <slot> [slot ...]
    if (args.size() < 3) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster addslots' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 解析并验证所有槽号
    std::vector<int> slots;
    for (size_t i = 2; i < args.size(); ++i) {
        try {
            int slot = std::stoi(args[i]);
            if (slot < 0 || slot > 16383) {
                return RespEncoder::encode_error("ERR invalid slot number");
            }
            slots.push_back(slot);
        } catch (...) {
            return RespEncoder::encode_error("ERR invalid slot number");
        }
    }

    if (slots.empty()) {
        return RespEncoder::encode_error("ERR no slots specified");
    }

    // 分配槽给本节点
    auto* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    int assigned = 0;
    for (int slot : slots) {
        // 检查槽是否已被其他节点拥有
        auto existing = state->getNodeForSlot(slot);
        if (existing && existing != my_node) {
            std::string err = "ERR slot " + std::to_string(slot) + " is already owned by another node";
            return RespEncoder::encode_error(err);
        }

        // 如果本节点尚未拥有该槽，则分配
        if (!my_node->hasSlot(slot)) {
            my_node->addSlot(slot);
            state->setNodeForSlot(slot, my_node);
            assigned++;
        }
    }

    LOG_INFO(CLUSTER, "Assigned %d slots to node %s", assigned, my_node->getName().c_str());
    return RespEncoder::encode_simple_string("OK");
}

std::string ClusterCommand::handleSlots(const std::vector<std::string>& args) {
    (void)args;  // 未使用参数

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    auto nodes = state->getAllNodes();
    if (nodes.empty()) {
        return RespEncoder::encode_array({});
    }

    // 按节点分组槽
    struct SlotRange {
        std::string ip;
        int port;
        std::vector<int> slots;
    };
    std::vector<SlotRange> node_slots;

    for (const auto& node : nodes) {
        SlotRange sr;
        sr.ip = node->getInfo().ip;
        sr.port = node->getInfo().port;
        sr.slots = node->getSlots();
        if (!sr.slots.empty()) {
            node_slots.push_back(sr);
        }
    }

    if (node_slots.empty()) {
        return RespEncoder::encode_array({});
    }

    // 排序：按 IP:Port
    std::sort(node_slots.begin(), node_slots.end(), [](const SlotRange& a, const SlotRange& b) {
        if (a.ip != b.ip) return a.ip < b.ip;
        return a.port < b.port;
    });

    // 构建响应
    // 格式：[[ip1, port1, slot1, slot2, ...], [ip2, port2, slot3, ...], ...]
    std::vector<std::vector<std::string>> nested;
    for (const auto& ns : node_slots) {
        std::vector<std::string> node_info;
        node_info.push_back(ns.ip);
        node_info.push_back(std::to_string(ns.port));
        for (int slot : ns.slots) {
            node_info.push_back(std::to_string(slot));
        }
        nested.push_back(std::move(node_info));
    }

    return RespEncoder::encode_nested_array(nested);
}

std::string ClusterCommand::handleDelSlots(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER DELSLOTS <slot> [slot ...]
    if (args.size() < 3) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster delslots' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 解析并验证所有槽号
    std::vector<int> slots;
    for (size_t i = 2; i < args.size(); ++i) {
        try {
            int slot = std::stoi(args[i]);
            if (slot < 0 || slot > 16383) {
                return RespEncoder::encode_error("ERR invalid slot number");
            }
            slots.push_back(slot);
        } catch (...) {
            return RespEncoder::encode_error("ERR invalid slot number");
        }
    }

    if (slots.empty()) {
        return RespEncoder::encode_error("ERR no slots specified");
    }

    auto* state = ClusterServer::instance().getState();
    if (!state) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    int removed = 0;
    for (int slot : slots) {
        // 只有本节点拥有该槽时才能删除
        if (my_node->hasSlot(slot)) {
            my_node->delSlot(slot);
            state->delSlot(slot);
            removed++;
        }
    }

    LOG_INFO(CLUSTER, "Removed %d slots from node %s", removed, my_node->getName().c_str());
    return RespEncoder::encode_simple_string("OK");
}

std::string ClusterCommand::handleSetSlot(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER SETSLOT <slot> <state> [node]
    // 状态可以是：MIGRATING | IMPORTING | NODE | STABLE | ...
    if (args.size() < 4) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster setslot' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 解析槽号
    int slot = -1;
    try {
        slot = std::stoi(args[2]);
        if (slot < 0 || slot > 16383) {
            return RespEncoder::encode_error("ERR invalid slot number");
        }
    } catch (...) {
        return RespEncoder::encode_error("ERR invalid slot number");
    }

    const std::string& state = args[3];
    auto* state_mgr = ClusterServer::instance().getState();
    if (!state_mgr) {
        return RespEncoder::encode_error("ERR cluster state not available");
    }

    // 处理不同的迁移状态
    if (state == "MIGRATING") {
        // CLUSTER SETSLOT <slot> MIGRATING <target_node>
        if (args.size() < 5) {
            return RespEncoder::encode_error("ERR syntax error, MIGRATING needs target node");
        }

        const std::string& target_node_name = args[4];
        auto target_node = state_mgr->getNode(target_node_name);
        if (!target_node) {
            return RespEncoder::encode_error("ERR target node not found");
        }

        ClusterServer::instance().setSlotMigrating(slot, target_node_name);
        LOG_INFO(CLUSTER, "Set slot %d to MIGRATING, target=%s", slot, target_node_name.c_str());
        return RespEncoder::encode_simple_string("OK");

    } else if (state == "IMPORTING") {
        // CLUSTER SETSLOT <slot> IMPORTING <source_node>
        if (args.size() < 5) {
            return RespEncoder::encode_error("ERR syntax error, IMPORTING needs source node");
        }

        const std::string& source_node_name = args[4];
        auto source_node = state_mgr->getNode(source_node_name);
        if (!source_node) {
            return RespEncoder::encode_error("ERR source node not found");
        }

        ClusterServer::instance().setSlotImporting(slot, source_node_name);
        LOG_INFO(CLUSTER, "Set slot %d to IMPORTING, source=%s", slot, source_node_name.c_str());
        return RespEncoder::encode_simple_string("OK");

    } else if (state == "NODE") {
        // CLUSTER SETSLOT <slot> NODE <node_name>
        // 迁移完成，正式设置槽归属
        if (args.size() < 5) {
            return RespEncoder::encode_error("ERR syntax error, NODE needs node name");
        }

        const std::string& node_name = args[4];
        auto node = state_mgr->getNode(node_name);
        if (!node) {
            return RespEncoder::encode_error("ERR node not found");
        }

        // 从原节点删除该槽
        auto old_owner = state_mgr->getNodeForSlot(slot);
        if (old_owner && old_owner != node) {
            old_owner->delSlot(slot);
        }

        // 设置新的槽归属并清除迁移状态
        ClusterServer::instance().setSlotOwner(slot, node_name);
        LOG_INFO(CLUSTER, "Set slot %d owner to %s", slot, node_name.c_str());
        return RespEncoder::encode_simple_string("OK");

    } else if (state == "STABLE") {
        // CLUSTER SETSLOT <slot> STABLE
        // 取消槽的迁移状态（一般在迁移取消时使用）
        ClusterServer::instance().clearSlotMigration(slot);
        LOG_INFO(CLUSTER, "Set slot %d to STABLE", slot);
        return RespEncoder::encode_simple_string("OK");

    } else {
        return RespEncoder::encode_error("ERR invalid slot state");
    }
}

std::string ClusterCommand::handleReplicate(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER REPLICATE <node_name>
    if (args.size() < 3) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster replicate' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 检查本节点是否已经是从节点
    if (my_node->isReplica()) {
        return RespEncoder::encode_error("ERR node is already a replica");
    }

    // 获取目标主节点名称
    const std::string& master_name = args[2];
    auto master = ClusterServer::instance().getState()->getNode(master_name);
    if (!master) {
        return RespEncoder::encode_error("ERR node not found: " + master_name);
    }

    if (!master->isMaster()) {
        return RespEncoder::encode_error("ERR target node is not a master");
    }

    // 设置复制关系
    if (ClusterServer::instance().setReplicaOf(master_name)) {
        LOG_INFO(CLUSTER, "Node %s is now replica of %s",
                 my_node->getName().c_str(), master_name.c_str());
        return RespEncoder::encode_simple_string("OK");
    }

    return RespEncoder::encode_error("ERR failed to set replica");
}

std::string ClusterCommand::handleFail(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER FAIL <node_name>
    if (args.size() < 3) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster fail' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    auto my_node = ClusterServer::instance().getMyNode();
    if (!my_node) {
        return RespEncoder::encode_error("ERR cluster not initialized");
    }

    // 获取目标节点名称
    const std::string& node_name = args[2];
    auto node = ClusterServer::instance().getState()->getNode(node_name);
    if (!node) {
        return RespEncoder::encode_error("ERR node not found: " + node_name);
    }

    // 手动标记节点为 FAIL
    ClusterServer::instance().markNodeAsFail(node_name);

    LOG_INFO(CLUSTER, "Node %s manually marked as FAIL by %s",
             node_name.c_str(), my_node->getName().c_str());
    return RespEncoder::encode_simple_string("OK");
}

bool ClusterCommand::isValidIp(const std::string& ip) {
    // 检查是否为空
    if (ip.empty()) {
        return false;
    }

    // 检查长度上限（IPv6 最大 45 个字符）
    if (ip.length() > 45) {
        return false;
    }

    // 检查是否是 IPv4 格式 (xxx.xxx.xxx.xxx)
    if (ip.find(':') == std::string::npos) {
        // IPv4 验证：必须恰好有 3 个点，每段 1-3 位数字，值 0-255
        int dot_count = 0;
        size_t last_pos = 0;  // 上一个段的结束位置

        for (size_t i = 0; i < ip.length(); ++i) {
            char c = ip[i];
            if (c == '.') {
                dot_count++;
                // 段长度 = 当前点位置 - 上一个段结束位置
                size_t seg_len = i - last_pos;
                if (seg_len < 1 || seg_len > 3) {
                    return false;
                }
                // 检查段内字符都是数字
                for (size_t j = last_pos; j < i; ++j) {
                    if (!std::isdigit(ip[j])) {
                        return false;
                    }
                }
                // 提取段值，检查范围 0-255
                std::string segment = ip.substr(last_pos, seg_len);
                if (segment.length() > 1 && segment[0] == '0') {
                    return false;  // 不允许前导零如 "01"
                }
                int value = std::atoi(segment.c_str());
                if (value < 0 || value > 255) {
                    return false;
                }
                last_pos = i + 1;  // 下一段的开始位置是点的下一个字符
            } else if (!std::isdigit(c)) {
                return false;  // IPv4 只能是数字和点
            }
        }

        // 检查最后一段
        size_t last_seg_len = ip.length() - last_pos;
        if (last_seg_len < 1 || last_seg_len > 3) {
            return false;
        }
        for (size_t j = last_pos; j < ip.length(); ++j) {
            if (!std::isdigit(ip[j])) {
                return false;
            }
        }
        std::string last_segment = ip.substr(last_pos);
        if (last_segment.length() > 1 && last_segment[0] == '0') {
            return false;
        }
        int value = std::atoi(last_segment.c_str());
        if (value < 0 || value > 255) {
            return false;
        }

        // 必须恰好 3 个点
        if (dot_count != 3) {
            return false;
        }

        return true;
    }

    // IPv6 验证：检查是否只包含十六进制字符和冒号
    for (char c : ip) {
        if (!std::isxdigit(c) && c != ':' && c != '.') {
            return false;
        }
    }

    // 简单检查：至少有一个冒号
    return ip.find(':') != std::string::npos;
}

std::string ClusterCommand::handleMigrate(const std::vector<std::string>& args) {
    // MIGRATE host port key dbid timeout [COPY | REPLACE]
    // 简化版本: CLUSTER MIGRATE host port key timeout [REPLACE]
    if (args.size() < 6) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster migrate' command");
    }

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR cluster mode is not enabled");
    }

    const std::string& host = args[2];

    // 解析端口号
    int port = 0;
    try {
        port = std::stoi(args[3]);
    } catch (...) {
        return RespEncoder::encode_error("ERR invalid port number");
    }

    if (port <= 0 || port > 65535) {
        return RespEncoder::encode_error("ERR invalid port number");
    }

    const std::string& key = args[4];

    // 解析超时
    int timeout = 0;
    try {
        timeout = std::stoi(args[5]);
    } catch (...) {
        return RespEncoder::encode_error("ERR invalid timeout");
    }

    // 检查是否有 REPLACE 标志
    bool replace = false;
    if (args.size() > 6 && args[6] == "REPLACE") {
        replace = true;
    }

    LOG_INFO(CLUSTER, "MIGRATE %s:%d key=%s timeout=%d replace=%d",
             host.c_str(), port, key.c_str(), timeout, replace);

    // 获取键值
    auto value_opt = GlobalStorage::instance().get(key);

    if (!value_opt.has_value()) {
        return RespEncoder::encode_error("ERR no such key");
    }

    const auto& value = value_opt.value();

    // 检查键的过期时间
    int64_t ttl_ms = -1;  // -1 表示永不过期
    if (GlobalStorage::instance().expire_dict().is_expired(key)) {
        // 键已过期，删除并返回错误
        GlobalStorage::instance().del(key);
        return RespEncoder::encode_error("ERR no such key");
    } else {
        // 获取剩余的 TTL
        int64_t remaining = GlobalStorage::instance().expire_dict().get_ttl(key);
        // get_ttl 返回 -2 表示没有过期时间（即永不过期）
        // 其他正数表示剩余的过期时间（毫秒）
        if (remaining > 0) {
            ttl_ms = remaining;
        }
        // 否则 ttl_ms 保持 -1（永不过期）
    }

    // 获取目标节点名称
    std::string target_name = host + ":" + std::to_string(port);

    // 连接到目标节点
    auto* conn = ClusterServer::instance().getConnection();
    if (!conn) {
        return RespEncoder::encode_error("ERR connection not available");
    }

    // 检查是否已连接，没有则建立连接
    if (!conn->is_node_connected(target_name)) {
        if (!conn->connect_to_node(target_name, host, port)) {
            return RespEncoder::encode_error("ERR failed to connect to target node");
        }
    }

    // 构建 RESTORE 命令发送到目标节点
    // RESTORE key ttl_ms serialized_value [REPLACE]
    // 注意：这里需要发送原始的序列化数据，实际实现中需要将 CacheObject 序列化为字节流

    // 获取序列化后的数据
    std::string serialized_value = value.serialize();

    // 构建 RESTORE 命令
    std::vector<std::string> restore_args;
    restore_args.push_back("RESTORE");
    restore_args.push_back(key);
    restore_args.push_back(std::to_string(ttl_ms));
    restore_args.push_back(serialized_value);
    if (replace) {
        restore_args.push_back("REPLACE");
    }

    // 发送 RESTORE 命令到目标节点
    if (!conn->send_command_to_node(target_name, restore_args)) {
        return RespEncoder::encode_error("ERR failed to send data to target node");
    }

    LOG_INFO(CLUSTER, "MIGRATE completed: key=%s -> %s:%d", key.c_str(), host.c_str(), port);
    return RespEncoder::encode_simple_string("OK");
}

} // namespace cc_server
