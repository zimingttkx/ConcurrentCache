#include "cluster_cmd.h"
#include "cluster/cluster_server.h"
#include "cluster/cluster_node.h"
#include "cluster/cluster_connection.h"
#include "cluster/replication_mgr.h"
#include "base/log.h"
#include "protocol/resp.h"
#include "cache/storage.h"
#include "datatype/object.h"
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace cc_server {

namespace {

/// @brief 把主机名解析成点分地址，解析不出来返回 false。成功时把 in_out 换成解析结果。
///
/// 直接替换成数字地址，是因为本仓库的节点身份与总线对账都以 "ip:port" 为单位
/// （见 ClusterConnection 里的 bus 身份规则）：让同一个节点在配置里叫
/// node-1、在身份表里叫 10.0.0.5，两条路径就会各说各话。
///
/// 先只试 AF_INET，再退到 AF_INET6。本服务目前绑的是 INADDR_ANY（IPv4），
/// 若主机名同时有 A 与 AAAA，拿 AAAA 去连会连一个自己根本没监听的地址，
/// 表现出来是"MEET 接受了但链路永远起不来"。
bool resolve_host_to_ip(std::string& in_out) {
    for (int family : {AF_INET, AF_INET6}) {
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res = nullptr;
        if (getaddrinfo(in_out.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            if (res != nullptr) {
                freeaddrinfo(res);
            }
            continue;
        }

        char buf[INET6_ADDRSTRLEN] = {0};
        bool ok = false;
        for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
            // 两个分支各写一遍 inet_ntop 的族参数，而不是把 p->ai_family 转手传进去：
            // ai_family 是 int，而 inet_ntop 收 sa_family_t，直接传会吃一条
            // -Wconversion；写成常量既没有转换也不会把不认识的族递下去。
            if (p->ai_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                ok = inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr;
            } else if (p->ai_family == AF_INET6) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
                ok = inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)) != nullptr;
            }
            if (ok) {
                break;
            }
        }
        freeaddrinfo(res);

        if (ok) {
            in_out = buf;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string ClusterCommand::execute(const std::vector<std::string>& args) {
    // 参数检查：CLUSTER <subcommand>
    if (args.size() < 2) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster' command");
    }

    std::string subcommand = args[1];
    // 转换为小写以兼容 Redis 协议（协议发送大写命令）
    for (char& c : subcommand) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

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

    // CLUSTER MEET <ip-or-hostname> <port>。Redis 7 起两种写法都接受，这里原来只认
    // isValidIp，任何主机名都直接回 "ERR invalid IP address" —— 在靠 DNS/容器名互相
    // 寻址的拓扑里标准写法一律连不上。daily 的 Full-cluster e2e 里"B 用主机名 MEET C"
    // 已经在断言这件事（#75），当时红在产品这一侧。
    //
    // 注意这与 Redis 一样有个副作用：inet_aton 风格的裸数字（"666"）也会被 getaddrinfo
    // 解释成一个地址，不是严格的语法校验。
    std::string ip = args[2];
    if (!isValidIp(ip) && !resolve_host_to_ip(ip)) {
        return RespEncoder::encode_error("ERR Invalid or unresolvable IP address or hostname");
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

    // 创建节点并添加到状态（默认为主节点）
    auto node = std::make_shared<ClusterNode>(name, ip, port, NodeRole::kMaster);

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
    // 总线身份对账拒了多少条。没有这个数，"复制被静默丢掉"和"什么都没发生"在
    // 协议面上长得一模一样。
    result += "cluster_bus_identity_rejected:" + std::to_string(state->bus_identity_rejections()) + "\n";
    // 入站链路数与因配额被关掉的连接数。没有这两个数，"对端反复连接被拒"与
    // "集群健康"在协议面上长得一样，而前者正是资源耗尽的前兆。
    auto* bus = ClusterServer::instance().getBus();
    result += "cluster_bus_inbound_links:" + std::to_string(bus ? bus->link_count() : 0) + "\n";
    result += "cluster_bus_inbound_refused:"
              + std::to_string(bus ? bus->inbound_refused() : 0) + "\n";
    result += "cluster_slots_assigned:" + std::to_string(slot_owner_count) + "\n";
    result += "cluster_my_node:" + my_node->getName() + "\n";
    int64_t epoch = my_node->getInfo().config_epoch;
    result += "cluster_current_epoch:" + std::to_string(epoch) + "\n";
    // 收发计数由 ClusterLink 在每一帧成功发出 / 每解出一条完整消息时递增，
    // 口径是"经过总线的消息条数"（gossip 与复制数据都算）。两个数长期不对称，
    // 通常意味着单向连通：我能发给某节点，但它发不到我。
    auto* st = ClusterServer::instance().getState();
    result += "cluster_stats_messages_sent:"
              + std::to_string(st ? st->bus_messages_sent() : 0) + "\n";
    result += "cluster_stats_messages_received:"
              + std::to_string(st ? st->bus_messages_received() : 0) + "\n";

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

        // 通过 cluster bus 向主节点发送复制同步请求
        auto* conn = ClusterServer::instance().getConnection();
        std::string repl_sync_msg = "REPLSYNC:" + my_node->getName();
        std::vector<std::string> sync_args = {repl_sync_msg};
        conn->send_command_to_node(master_name, sync_args);

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

// 严格整数：整个串都被吃掉才算数。stoi 读前缀就返回，"5000abc" 会变成 5000 ——
// 对一个决定阻塞事件循环多久的参数来说，这种"猜个差不多的值"不能收。
namespace {
    bool parse_bounded_integer(const std::string& text, long long& out) {
        const char* first = text.data();
        const char* last = first + text.size();
        const std::from_chars_result r = std::from_chars(first, last, out);
        return r.ec == std::errc() && r.ptr == last;
    }
}  // namespace

std::string ClusterCommand::handleMigrate(const std::vector<std::string>& args) {
    // 本项目的语法（比 Redis 顶层 MIGRATE 少一个 destination-db 字段）：
    //     CLUSTER MIGRATE host port key timeout [REPLACE]
    // Redis 的 `MIGRATE host port key dbid timeout ...` 顶层命令没有注册，别照它写。
    if (args.size() < 6) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'cluster migrate' command");
    }

    // 超时先校验，再管"集群开没开"：这个值现在真的决定阻塞多久（#92 起 MIGRATE 要等
    // 目标节点的回复才删源键），所以不能收下任意整数；而放在 enabled 检查之前，
    // 是为了让参数错误在单机模式下也能被契约用例抓到。
    long long parsed_timeout = 0;
    if (!parse_bounded_integer(args[5], parsed_timeout)) {
        return RespEncoder::encode_error("ERR timeout is not an integer or out of range");
    }
    // 命令跑在 SubReactor 的事件循环线程上：等回复期间这条 reactor 上的**所有**连接
    // 都停着。所以上限必须有，否则一个 999999999 就能把整条 reactor 冻十几天 ——
    // #92 之前 timeout 被完全忽略，也就没有这个风险，是这次引入的。
    constexpr long long kMaxMigrateTimeoutMs = 60000;
    if (parsed_timeout <= 0 || parsed_timeout > kMaxMigrateTimeoutMs) {
        return RespEncoder::encode_error(
            "ERR timeout must be between 1 and 60000 milliseconds");
    }
    const int timeout = static_cast<int>(parsed_timeout);

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

    // 把 RESTORE 发过去，并**等目标节点的回复**。
    //
    // 两处原来都不对：
    //   1. 以前用 send_command_to_node()，它只告诉你"发出去了没有"，而回复被
    //      接收端丢弃 —— 于是这条命令其实从来没把数据搬走过（RESTORE 的参数
    //      被拆成多个总线参数，接收端只执行 args[0] 那个裸的 "RESTORE"），
    //      客户端却收到 +OK。现在整条命令按复制键流那种 RESP 数组发，并且
    //      要求回复。
    //   2. 就算数据送到了，也不能凭空删源键：目标可能回 -BUSYKEY（键已存在且
    //      没带 REPLACE）。"目标没收下、源已经删了"是数据丢失，比留下重复键更糟。
    //      所以只有确认 +OK 之后才删。timeout 是这次往返的上限，到点报错、源键不动。
    std::string target_reply;
    if (!conn->send_command_and_wait(target_name, restore_args, timeout, target_reply)) {
        return RespEncoder::encode_error(
            "ERR MIGRATE timed out or could not reach the target node; the source key was kept");
    }
    if (target_reply.compare(0, 3, "+OK") != 0) {
        // 把对端的错误原样转给客户端（-BUSYKEY ... 之类），但别顺手删源键
        if (!target_reply.empty() && target_reply[0] == '-') {
            return RespEncoder::encode_error("ERR target node replied: " +
                                             target_reply.substr(1, target_reply.find("\r\n") - 1));
        }
        return RespEncoder::encode_error("ERR target node returned an unexpected reply");
    }

    // 目标确认收下了，才动源键（Redis 的 MIGRATE 语义：默认就是移动，不是复制）
    const bool removed = GlobalStorage::instance().del(key);
    LOG_INFO(CLUSTER, "MIGRATE completed: key=%s -> %s:%d source_removed=%d",
             key.c_str(), host.c_str(), port, removed ? 1 : 0);
    return RespEncoder::encode_simple_string("OK");
}

} // namespace cc_server
