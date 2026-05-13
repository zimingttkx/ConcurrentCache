// cluster_server.cpp
#include "cluster_server.h"
#include "base/config.h"
#include "base/log.h"
#include "protocol/resp.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
namespace cc_server {

ClusterServer& ClusterServer::instance() {
    static ClusterServer instance;
    return instance;
}

void ClusterServer::init() {
    // 读取配置，判断是否启用集群
    enabled_ = Config::instance().clusterEnabled();

    if (!enabled_) {
        LOG_INFO(CLUSTER, "Cluster mode is disabled");
        return;
    }

    LOG_INFO(CLUSTER, "Initializing cluster mode...");

    // 获取本节点配置
    std::string my_ip = "127.0.0.1";  // TODO: 从配置获取
    int my_port = Config::instance().getInt("port", 6379);

    // 构建节点名称（使用 ip:port 作为唯一标识）
    std::string my_name = my_ip + ":" + std::to_string(my_port);

    // 创建本节点（默认作为主节点）
    my_node_ = std::make_shared<ClusterNode>(my_name, my_ip, my_port, NodeRole::kMaster);

    // 设置本节点名称到集群状态
    state_.setMyNodeName(my_name);

    // 将本节点添加到集群状态
    state_.addNode(my_node_);

    // 初始化连接管理器
    connection_.init();
    connection_.set_state(&state_);

    // 初始化 Gossip 协议
    gossip_.init(&state_);

    // 设置连接回调 - 当收到 PONG 时将对端节点添加到状态
    // 这完成 MEET 命令的三次握手:发送 MEET -> 收到 PONG -> 添加对端节点
    connection_.set_node_connected_callback([this](const std::string& node_name) {
        // 解析 node_name (格式: ip:port)
        auto colon_pos = node_name.find(':');
        if (colon_pos == std::string::npos) {
            return;
        }
        std::string ip = node_name.substr(0, colon_pos);
        int port = std::stoi(node_name.substr(colon_pos + 1));

        // 检查节点是否已存在
        if (state_.getNode(node_name)) {
            return;
        }

        // 创建节点并添加到状态
        auto node = std::make_shared<ClusterNode>(node_name, ip, port, NodeRole::kNodeUnknown);
        node->addFlags(static_cast<uint64_t>(NodeFlags::kHandshake));
        state_.addNode(node);
        LOG_INFO(CLUSTER, "Added node via PONG: %s (%s:%d)", node_name.c_str(), ip.c_str(), port);
    });

    // 设置断开回调
    connection_.set_node_disconnected_callback([this](const std::string& node_name) {
        state_.delNode(node_name);
        LOG_INFO(CLUSTER, "Removed node: %s", node_name.c_str());
    });

    // 设置 MEET 回调 - 当收到其他节点发来的 MEET 消息时，将其添加到状态
    connection_.set_meet_callback([this](const std::string& ip, int port) {
        std::string name = ip + ":" + std::to_string(port);

        // 检查节点是否已存在
        if (state_.getNode(name)) {
            return;
        }

        // 创建握手节点并添加到状态
        auto node = std::make_shared<ClusterNode>(name, ip, port, NodeRole::kNodeUnknown);
        node->addFlags(static_cast<uint64_t>(NodeFlags::kHandshake));
        state_.addNode(node);
        LOG_INFO(CLUSTER, "Added node via MEET: %s (%s:%d)", name.c_str(), ip.c_str(), port);
    });

    LOG_INFO(CLUSTER, "Cluster initialized: node_name=%s, enabled=%s",
             my_name.c_str(), enabled_ ? "yes" : "no");
}

void ClusterServer::start() {
    if (!enabled_) return;

    running_ = true;
    connection_.start_heartbeat();
    LOG_INFO(CLUSTER, "Cluster server started");
}

void ClusterServer::stop() {
    if (!enabled_) return;

    connection_.stop_heartbeat();
    running_ = false;
    LOG_INFO(CLUSTER, "Cluster server stopped");
}

void ClusterServer::on_timer() {
    if (!enabled_ || !running_) return;
    connection_.on_timer();
    // 执行故障转移状态机
    executeFailover();
}

// CRC16 查找表（预计算，必须在 crc16 函数之前）
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

// CRC16 算法（与 Redis 兼容）
// Redis 使用 CRC16 算法将 key 映射到 16384 个槽
static uint16_t crc16(const char* buf, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc = static_cast<uint16_t>((crc << 8) ^ crc16_table[(crc ^ buf[i]) & 0xFF]);
    }
    return crc;
}

int ClusterServer::keyToSlot(const std::string& key) const {
    // 防御性检查
    assert(!key.empty() && "keyToSlot: key is empty");

    // Redis 槽算法：查找 {tag}，只对 tag 进行哈希
    // 格式如：{tag}.anything
    int start = -1;
    int end = -1;

    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] == '{') {
            start = static_cast<int>(i) + 1;
        } else if (key[i] == '}' && start != -1) {
            end = static_cast<int>(i);
            break;
        }
    }

    // 如果找到有效的 tag，使用 tag 计算槽
    if (start != -1 && end != -1 && end > start) {
        return crc16(key.c_str() + start, end - start) & 0x3FFF;
    }

    // 否则使用整个 key 计算槽
    return crc16(key.c_str(), static_cast<int>(key.size())) & 0x3FFF;
}

std::shared_ptr<ClusterNode> ClusterServer::getNodeByKey(const std::string& key) const {
    int slot = keyToSlot(key);
    return getNodeBySlot(slot);
}

std::shared_ptr<ClusterNode> ClusterServer::getNodeBySlot(int slot) const {
    // 防御性检查
    assert(slot >= 0 && slot <= 16383 && "getNodeBySlot: slot out of range");

    return state_.getNodeForSlot(slot);
}

std::string ClusterServer::getMyNodeName() const {
    if (my_node_) {
        return my_node_->getName();
    }
    return "";
}

void ClusterServer::setSlotMigrating(int slot, const std::string& target_node) {
    assert(slot >= 0 && slot <= 16383 && "setSlotMigrating: slot out of range");
    assert(!target_node.empty() && "setSlotMigrating: target_node is empty");

    // 清除之前的迁移状态
    state_.clearSlotMigration(slot);

    // 设置新的迁出状态
    state_.setSlotMigrating(slot, target_node);
    LOG_INFO(CLUSTER, "Set slot %d to MIGRATING, target=%s", slot, target_node.c_str());
}

void ClusterServer::setSlotImporting(int slot, const std::string& source_node) {
    assert(slot >= 0 && slot <= 16383 && "setSlotImporting: slot out of range");
    assert(!source_node.empty() && "setSlotImporting: source_node is empty");

    // 清除之前的迁移状态
    state_.clearSlotMigration(slot);

    // 设置新的迁入状态
    state_.setSlotImporting(slot, source_node);
    LOG_INFO(CLUSTER, "Set slot %d to IMPORTING, source=%s", slot, source_node.c_str());
}

void ClusterServer::clearSlotMigration(int slot) {
    assert(slot >= 0 && slot <= 16383 && "clearSlotMigration: slot out of range");

    state_.clearSlotMigration(slot);
    LOG_INFO(CLUSTER, "Cleared slot %d migration state", slot);
}

void ClusterServer::setSlotOwner(int slot, const std::string& node_name) {
    assert(slot >= 0 && slot <= 16383 && "setSlotOwner: slot out of range");
    assert(!node_name.empty() && "setSlotOwner: node_name is empty");

    auto node = state_.getNode(node_name);
    if (!node) {
        LOG_ERROR(CLUSTER, "Cannot set slot owner: node not found: %s", node_name.c_str());
        return;
    }

    // 清除迁移状态
    state_.clearSlotMigration(slot);

    // 设置槽归属
    state_.setNodeForSlot(slot, node);

    // 更新节点的槽列表
    node->addSlot(slot);

    LOG_INFO(CLUSTER, "Set slot %d owner to %s", slot, node_name.c_str());
}

bool ClusterServer::isSlotMigrating(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "isSlotMigrating: slot out of range");
    return state_.isSlotMigrating(slot);
}

bool ClusterServer::isSlotImporting(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "isSlotImporting: slot out of range");
    return state_.isSlotImporting(slot);
}

SlotMigrationInfo ClusterServer::getSlotMigrationInfo(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "getSlotMigrationInfo: slot out of range");
    return state_.getSlotMigrationInfo(slot);
}

std::shared_ptr<ClusterNode> ClusterServer::getSlotMigrationTarget(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "getSlotMigrationTarget: slot out of range");

    SlotMigrationInfo info = state_.getSlotMigrationInfo(slot);
    if (info.status == SlotMigrationStatus::kMigrating && !info.target_node.empty()) {
        return state_.getNode(info.target_node);
    }
    return nullptr;
}

std::shared_ptr<ClusterNode> ClusterServer::getSlotMigrationSource(int slot) const {
    assert(slot >= 0 && slot <= 16383 && "getSlotMigrationSource: slot out of range");

    SlotMigrationInfo info = state_.getSlotMigrationInfo(slot);
    if (info.status == SlotMigrationStatus::kImporting && !info.source_node.empty()) {
        return state_.getNode(info.source_node);
    }
    return nullptr;
}

std::string ClusterServer::checkRedirect(const std::string& key) const {
    // 计算 key 对应的槽
    int slot = keyToSlot(key);

    // 检查槽是否正在迁移（迁出状态）
    if (state_.isSlotMigrating(slot)) {
        // 槽正在迁出，返回 ASK 重定向
        SlotMigrationInfo info = state_.getSlotMigrationInfo(slot);
        auto target = state_.getNode(info.target_node);
        if (target) {
            std::string redirect = "ASK " + std::to_string(slot) + " " +
                                   target->getInfo().ip + ":" + std::to_string(target->getInfo().port);
            LOG_DEBUG(CLUSTER, "ASK redirect for key '%s' slot %d -> %s",
                     key.c_str(), slot, redirect.c_str());
            return RespEncoder::encode_error(redirect);
        }
    }

    // 检查槽是否正在迁入
    if (state_.isSlotImporting(slot)) {
        // 槽正在迁入，检查本地是否有数据
        // 如果本地没有数据，则返回 ASK 重定向到源节点
        SlotMigrationInfo info = state_.getSlotMigrationInfo(slot);
        auto source = state_.getNode(info.source_node);
        if (source) {
            // TODO: 实际应该检查本地是否有该 key 的数据
            // 如果没有，返回 ASK 重定向
            // 这里暂时返回空，让命令正常执行
            LOG_DEBUG(CLUSTER, "Slot %d is IMPORTING from %s, key '%s'",
                     slot, info.source_node.c_str(), key.c_str());
        }
    }

    // 检查槽归属
    auto owner = state_.getNodeForSlot(slot);
    if (!owner) {
        // 槽没有归属，可能是刚加入集群的新节点
        // 允许执行命令
        return "";
    }

    // 如果槽归属不是本节点，返回 MOVED 重定向
    if (owner->getName() != getMyNodeName()) {
        std::string redirect = "MOVED " + std::to_string(slot) + " " +
                               owner->getInfo().ip + ":" + std::to_string(owner->getInfo().port);
        LOG_DEBUG(CLUSTER, "MOVED redirect for key '%s' slot %d -> %s",
                 key.c_str(), slot, redirect.c_str());
        return RespEncoder::encode_error(redirect);
    }

    return "";
}

bool ClusterServer::setReplicaOf(const std::string& master_name) {
    assert(!master_name.empty() && "setReplicaOf: master_name is empty");

    if (!enabled_) {
        LOG_ERROR(CLUSTER, "Cannot set replica: cluster mode is disabled");
        return false;
    }

    // 获取主节点
    auto master = state_.getNode(master_name);
    if (!master) {
        LOG_ERROR(CLUSTER, "Cannot set replica: master node not found: %s", master_name.c_str());
        return false;
    }

    if (!master->isMaster()) {
        LOG_ERROR(CLUSTER, "Cannot set replica: node %s is not a master", master_name.c_str());
        return false;
    }

    // 获取主节点的 IP 和端口
    const auto& master_info = master->getInfo();
    std::string master_ip = master_info.ip;
    int master_port = master_info.port;

    // 设置本节点的复制信息
    my_node_->setRole(NodeRole::kReplica);
    my_node_->setReplicationState(ReplicationState::kConnect);
    my_node_->setMaster(master_ip, master_port);  // 设置 info_.replicaof_ip/port
    my_node_->setMasterNode(master);               // 设置 master_node_ 指针

    // 更新主节点的从节点列表
    state_.addReplica(master_name, my_node_);

    LOG_INFO(CLUSTER, "Set replica of master %s (%s:%d)", master_name.c_str(), master_ip.c_str(), master_port);
    return true;
}

void ClusterServer::clearReplicaOf() {
    if (!isReplica()) {
        return;
    }

    auto master = my_node_->getMasterNode();
    if (master) {
        state_.removeReplica(master->getName(), getMyNodeName());
    }

    my_node_->setRole(NodeRole::kMaster);
    my_node_->setReplicationState(ReplicationState::kNone);
    my_node_->setMasterNode(nullptr);

    // 清除 info_.replicaof_ip/port
    my_node_->getInfo().replicaof_ip.clear();
    my_node_->getInfo().replicaof_port = 0;

    LOG_INFO(CLUSTER, "Cleared replica relationship, now master");
}

bool ClusterServer::isReplica() const {
    if (!my_node_) return false;
    return my_node_->isReplica();
}

std::shared_ptr<ClusterNode> ClusterServer::getMyMaster() const {
    if (!my_node_) return nullptr;
    return my_node_->getMasterNode();
}

std::vector<std::shared_ptr<ClusterNode>> ClusterServer::getMyReplicas() const {
    if (!my_node_) return {};
    return state_.getReplicas(my_node_->getName());
}

void ClusterServer::handleNodeTimeout(const std::string& node_name) {
    if (!enabled_ || !running_) return;
    if (node_name.empty()) return;

    auto node = state_.getNode(node_name);
    if (!node) return;

    // 如果节点已经是 FAIL 状态，不再处理
    if (node->isFailed()) return;

    // 增加失败计数
    node->incrementFailureCount();

    // 如果失败计数超过阈值，标记为疑似下线
    const int max_ping_failures = 3;  // TODO: 从配置读取
    if (node->getFailureCount() >= max_ping_failures) {
        state_.markNodeAsPfail(node_name);
        LOG_WARN(CLUSTER, "Node %s ping timeout, marked as PFAIL (failures=%d)",
                 node_name.c_str(), node->getFailureCount());
    }
}

void ClusterServer::handleNodeRecovery(const std::string& node_name) {
    if (!enabled_) return;
    if (node_name.empty()) return;

    auto node = state_.getNode(node_name);
    if (!node) return;

    // 重置失败计数
    node->resetFailureCount();

    // 如果节点是疑似下线状态，清除
    if (node->isPfailed()) {
        state_.clearNodePfail(node_name);
        LOG_INFO(CLUSTER, "Node %s recovered, cleared PFAIL", node_name.c_str());
    }

    // 如果节点是客观下线状态，清除
    if (node->isFailed()) {
        state_.clearNodeFail(node_name);
        LOG_INFO(CLUSTER, "Node %s recovered, cleared FAIL", node_name.c_str());
    }
}

bool ClusterServer::checkFailQuorum(const std::string& node_name) {
    if (!enabled_) return false;
    if (node_name.empty()) return false;

    auto node = state_.getNode(node_name);
    if (!node) return false;

    // 如果不是疑似下线，不检查客观下线
    if (!node->isPfailed()) return false;

    // 获取集群中存活的主节点数量
    auto nodes = state_.getAllNodes();
    int live_master_count = 0;
    for (const auto& n : nodes) {
        // 只计算主节点
        if (n->isMaster() && n->isConnected() && !n->isFailed()) {
            live_master_count++;
        }
    }

    // 多数原则：超过一半的主节点认为该节点下线
    int quorum = (live_master_count / 2) + 1;
    const int min_quorum = 2;  // 最小 quorum 值

    // TODO: 实际上应该通过 Gossip 协议收集其他节点对同一节点的 pfailing 报告
    // 这里简化处理：如果本节点认为节点疑似下线，且存活主节点数 >= 最小 quorum
    if (live_master_count >= min_quorum) {
        LOG_INFO(CLUSTER, "Node %s fail quorum reached: live_masters=%d, quorum=%d",
                 node_name.c_str(), live_master_count, quorum);
        return true;
    }

    return false;
}

void ClusterServer::markNodeAsFail(const std::string& node_name) {
    if (!enabled_) return;
    if (node_name.empty()) return;

    auto node = state_.getNode(node_name);
    if (!node) return;

    // 直接标记为客观下线
    state_.markNodeAsFail(node_name);

    // 构建 FAIL 广播消息
    GossipMsg gossip_msg;
    gossip_msg.type = GossipType::kFail;
    gossip_msg.sender_name = getMyNodeName();
    gossip_msg.sender_epoch = my_node_->getInfo().config_epoch;

    GossipNodeInfo info;
    info.name = node->getName();
    info.ip = node->getInfo().ip;
    info.port = node->getInfo().port;
    info.flags = node->getFlags();
    info.role = node->isMaster() ? 0 : 1;
    gossip_msg.nodes.push_back(info);

    // 通过连接管理器广播 FAIL 消息
    connection_.broadcast_gossip(gossip_msg);

    LOG_INFO(CLUSTER, "Node %s manually marked as FAIL", node_name.c_str());
}

// ==================== 故障转移相关实现 ====================

bool ClusterServer::startFailover(const std::string& master_name) {
    if (!enabled_) return false;
    if (!isReplica()) return false;  // 只有从节点能发起故障转移

    // 获取主节点
    auto master = state_.getNode(master_name);
    if (!master) {
        LOG_ERROR(CLUSTER, "Cannot start failover: master not found: %s", master_name.c_str());
        return false;
    }

    // 检查主节点是否已下线
    if (!master->isFailed()) {
        LOG_WARN(CLUSTER, "Cannot start failover: master %s is not failed", master_name.c_str());
        return false;
    }

    // 检查本节点是否已在故障转移中
    if (my_node_->getFailoverState() != FailoverState::kNoFailover) {
        LOG_WARN(CLUSTER, "Already in failover state: %d", static_cast<int>(my_node_->getFailoverState()));
        return false;
    }

    // 增加 failover_epoch
    int64_t new_epoch = master->getInfo().config_epoch + 1;
    my_node_->setFailoverEpoch(new_epoch);
    my_node_->setFailoverState(FailoverState::kWaitStart);

    LOG_INFO(CLUSTER, "Started failover for master %s (epoch=%ld)", master_name.c_str(), new_epoch);
    return true;
}

bool ClusterServer::shouldStartFailover() {
    if (!enabled_) return false;
    if (!isReplica()) return false;

    auto master = getMyMaster();
    if (!master) return false;

    // 主节点必须已下线
    if (!master->isFailed()) return false;

    // 本节点必须未在故障转移中
    if (my_node_->getFailoverState() != FailoverState::kNoFailover) return false;

    // 检查复制偏移量是否足够新（与主节点的差距在可接受范围内）
    // 这里简化处理：只要主节点FAIL且本节点数据不是太旧就允许
    return true;
}

bool ClusterServer::handleFailoverAuthRequest(const std::string& replica_name, int64_t epoch, int64_t offset) {
    if (!enabled_) return false;

    // 只能给主节点投票
    if (!my_node_->isMaster()) return false;

    auto replica = state_.getNode(replica_name);
    if (!replica) return false;

    // 检查 epoch 是否更新
    if (epoch <= my_node_->getFailoverEpoch()) {
        LOG_WARN(CLUSTER, "Rejecting auth request from %s: epoch %ld <= my epoch %ld",
                 replica_name.c_str(), epoch, my_node_->getFailoverEpoch());
        return false;
    }

    // 检查 replica 的数据是否够新（复制偏移量接近本节点）
    // 这里简化处理：直接批准投票
    // TODO: 实际应该比较 offset 与 master_repl_offset_

    // 记录投票
    my_node_->addVote(replica_name, epoch, offset);
    my_node_->setFailoverEpoch(epoch);

    LOG_INFO(CLUSTER, "Granted vote to %s for failover (epoch=%ld, offset=%ld)",
             replica_name.c_str(), epoch, offset);
    return true;
}

void ClusterServer::handleFailoverAuthAck(const std::string& node_name) {
    // 从节点收到主节点的投票确认
    LOG_INFO(CLUSTER, "Received failover auth ack from %s", node_name.c_str());
    // 投票计数由 ClusterNode 自行管理
}

std::vector<std::shared_ptr<ClusterNode>> ClusterServer::getReplicasForMaster(const std::string& master_name) const {
    return state_.getReplicas(master_name);
}

void ClusterServer::executeFailover() {
    if (!enabled_) return;
    if (!isReplica()) return;

    auto master = getMyMaster();
    if (!master) return;

    FailoverState state = my_node_->getFailoverState();

    switch (state) {
        case FailoverState::kNoFailover:
            // 检查是否应该开始故障转移
            if (shouldStartFailover()) {
                startFailover(master->getName());
            }
            break;

        case FailoverState::kWaitStart:
            // 等待开始状态，切换到等待投票
            my_node_->setFailoverState(FailoverState::kWaitAuth);
            LOG_INFO(CLUSTER, "Failover state: WaitAuth");
            break;

        case FailoverState::kWaitAuth: {
            // 等待投票状态
            // 计算需要的票数（多数派）
            auto all_nodes = state_.getAllNodes();
            int master_count = 0;
            for (const auto& node : all_nodes) {
                if (node->isMaster() && !node->isFailed()) {
                    master_count++;
                }
            }
            int needed_votes = (master_count / 2) + 1;

            int current_votes = my_node_->getVoteCount();
            LOG_INFO(CLUSTER, "Failover state: WaitAuth, votes=%d, needed=%d", current_votes, needed_votes);

            if (current_votes >= needed_votes) {
                // 获得足够票数，开始执行故障转移
                my_node_->setFailoverState(FailoverState::kFailoverInProgress);
                LOG_INFO(CLUSTER, "Failover state: FailoverInProgress");
            }
            break;
        }

        case FailoverState::kFailoverInProgress:
            // 执行故障转移：接管主节点的槽
            completeFailover();
            break;

        case FailoverState::kFailoverCompleted:
            // 故障转移已完成
            LOG_INFO(CLUSTER, "Failover completed successfully");
            break;

        case FailoverState::kFailoverTimeout:
            // 故障转移超时，重置状态
            my_node_->setFailoverState(FailoverState::kNoFailover);
            my_node_->clearVotes();
            LOG_WARN(CLUSTER, "Failover timeout, resetting state");
            break;
    }
}

void ClusterServer::completeFailover() {
    if (!enabled_) return;
    if (!isReplica()) return;

    auto master = getMyMaster();
    if (!master) return;

    // 获取主节点负责的所有槽
    std::vector<int> master_slots = master->getSlots();

    // 将槽转移到本节点
    for (int slot : master_slots) {
        setSlotOwner(slot, getMyNodeName());
    }

    // 清除复制关系，本节点变为主节点
    clearReplicaOf();

    // 更新故障转移状态
    my_node_->setFailoverState(FailoverState::kFailoverCompleted);

    LOG_INFO(CLUSTER, "Failover completed: took over %zu slots from %s",
             master_slots.size(), master->getName().c_str());
}

} // namespace cc_server
