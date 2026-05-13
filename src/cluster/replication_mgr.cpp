// replication_mgr.cpp
#include "replication_mgr.h"
#include "cluster_server.h"
#include "base/log.h"
#include "cache/storage.h"
#include <algorithm>
#include <random>
#include <sstream>

namespace cc_server {

ReplicationMgr::ReplicationMgr()
    : state_(nullptr) {
    repl_buffer_.reserve(1024);
    LOG_INFO(CLUSTER, "ReplicationMgr created");
}

ReplicationMgr::~ReplicationMgr() {
    LOG_INFO(CLUSTER, "ReplicationMgr destroyed");
}

ReplicationMgr& ReplicationMgr::instance() {
    static ReplicationMgr instance;
    return instance;
}

void ReplicationMgr::init(ClusterState* state) {
    state_ = state;

    // 生成主节点运行 ID
    master_runid_ = generate_runid();
    master_repl_offset_.store(0);

    LOG_INFO(CLUSTER, "ReplicationMgr initialized with runid=%s", master_runid_.c_str());
}

std::string ReplicationMgr::generate_runid() {
    // 生成 40 字符的随机十六进制字符串（类似 Redis）
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    for (int i = 0; i < 40; i++) {
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

void ReplicationMgr::add_replica(const std::string& name, const std::string& ip, int port,
                                  std::shared_ptr<ClusterNode> node) {
    std::lock_guard<std::shared_mutex> lock(replicas_mutex_);

    auto replica = std::make_shared<ReplicaInfo>();
    replica->name = name;
    replica->ip = ip;
    replica->port = port;
    replica->node = node;
    replica->repl_offset = 0;
    replica->last_ack_time = 0;
    replica->sync_state = SyncState::kWaiting;

    replicas_[name] = replica;

    LOG_INFO(CLUSTER, "Added replica: name=%s, ip=%s, port=%d, total_replicas=%zu",
             name.c_str(), ip.c_str(), port, replicas_.size());
}

void ReplicationMgr::remove_replica(const std::string& name) {
    std::lock_guard<std::shared_mutex> lock(replicas_mutex_);

    auto it = replicas_.find(name);
    if (it != replicas_.end()) {
        replicas_.erase(it);
        LOG_INFO(CLUSTER, "Removed replica: name=%s, remaining_replicas=%zu",
                 name.c_str(), replicas_.size());
    }
}

std::vector<std::shared_ptr<ReplicaInfo>> ReplicationMgr::get_all_replicas() const {
    std::shared_lock<std::shared_mutex> lock(replicas_mutex_);

    std::vector<std::shared_ptr<ReplicaInfo>> result;
    result.reserve(replicas_.size());

    for (const auto& [name, replica] : replicas_) {
        result.push_back(replica);
    }

    return result;
}

void ReplicationMgr::add_to_replication_buffer(const std::string& command) {
    std::lock_guard<std::mutex> lock(repl_buffer_mutex_);

    ReplicationBufferEntry entry;
    entry.command = command;
    entry.offset = master_repl_offset_.load();

    repl_buffer_.push_back(entry);

    // 更新主节点复制偏移量
    master_repl_offset_.store(entry.offset + command.size());

    // 如果缓冲区太大，清理旧数据
    size_t total_size = 0;
    for (const auto& e : repl_buffer_) {
        total_size += e.command.size();
    }
    if (total_size > kReplicationBufferSize) {
        cleanup_replication_buffer();
    }
}

void ReplicationMgr::replicate_command(const std::string& command) {
    // 添加到复制缓冲区
    add_to_replication_buffer(command);

    // TODO: 实际发送给副本的逻辑
    // 实际发送由 ClusterConnection 处理
}

std::vector<ReplicationBufferEntry> ReplicationMgr::get_replication_commands(int64_t from_offset) {
    std::lock_guard<std::mutex> lock(repl_buffer_mutex_);

    std::vector<ReplicationBufferEntry> result;

    for (const auto& entry : repl_buffer_) {
        if (entry.offset >= from_offset) {
            result.push_back(entry);
        }
    }

    return result;
}

void ReplicationMgr::cleanup_replication_buffer() {
    if (repl_buffer_.empty()) {
        return;
    }

    // 保留最近的一半数据
    size_t keep_count = repl_buffer_.size() / 2;
    int64_t new_start_offset = repl_buffer_[keep_count].offset;

    repl_buffer_.erase(repl_buffer_.begin(), repl_buffer_.begin() + keep_count);
    repl_buffer_start_offset_.store(new_start_offset);

    LOG_DEBUG(CLUSTER, "Cleaned up replication buffer, new_start_offset=%ld, remaining_entries=%zu",
              new_start_offset, repl_buffer_.size());
}

void ReplicationMgr::update_replica_ack_offset(const std::string& replica_name, int64_t offset) {
    std::shared_lock<std::shared_mutex> lock(replicas_mutex_);

    auto it = replicas_.find(replica_name);
    if (it != replicas_.end()) {
        it->second->repl_offset = offset;
        it->second->last_ack_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

bool ReplicationMgr::send_rdb_to_replica(const std::string& replica_name) {
    if (rdb_send_in_progress_.load()) {
        LOG_WARN(CLUSTER, "RDB send already in progress");
        return false;
    }

    rdb_send_in_progress_.store(true);

    // 获取副本信息
    std::shared_ptr<ReplicaInfo> replica;
    {
        std::shared_lock<std::shared_mutex> lock(replicas_mutex_);
        auto it = replicas_.find(replica_name);
        if (it == replicas_.end()) {
            rdb_send_in_progress_.store(false);
            return false;
        }
        replica = it->second;
    }

    // 标记正在发送 RDB
    replica->sync_state = SyncState::kSendingRdb;

    // TODO: 使用 RdbPersistence 生成 RDB 文件并发送
    // 目前简化处理：直接序列化所有数据并发送

    // 标记 RDB 发送完成
    replica->sync_state = SyncState::kSendingkv;

    rdb_send_in_progress_.store(false);

    LOG_INFO(CLUSTER, "RDB sent to replica: %s", replica_name.c_str());
    return true;
}

void ReplicationMgr::set_master(const std::string& ip, int port, const std::string& master_runid) {
    master_ip_ = ip;
    master_port_ = port;
    master_runid_ = master_runid;
    sync_state_.store(SyncState::kWaiting);

    LOG_INFO(CLUSTER, "Set master: ip=%s, port=%d, runid=%s",
             ip.c_str(), port, master_runid.c_str());
}

} // namespace cc_server