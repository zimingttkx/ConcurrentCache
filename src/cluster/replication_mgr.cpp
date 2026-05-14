// replication_mgr.cpp
#include "replication_mgr.h"
#include "cluster_server.h"
#include "cluster_connection.h"
#include "base/log.h"
#include "cache/storage.h"
#include "protocol/resp.h"
#include "command/command_factory.h"
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
    master_repl_offset_.store(entry.offset + static_cast<int64_t>(command.size()));

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
    static std::atomic<int64_t> rep_cmd_seq{0};
    int64_t seq = rep_cmd_seq.fetch_add(1);

    // 添加到复制缓冲区
    add_to_replication_buffer(command);

    // 推送给所有处于增量同步状态的副本
    auto replicas = get_all_replicas();
    LOG_INFO(CLUSTER, "REPL-SEND[%ld] cmd=%s replicas=%zu",
             seq, command.c_str(), replicas.size());
    for (auto& replica : replicas) {
        if (replica->sync_state == SyncState::kSendingkv ||
            replica->sync_state == SyncState::kConnected) {
            LOG_INFO(CLUSTER, "REPL-SEND[%ld] to replica=%s state=%d",
                     seq, replica->name.c_str(), static_cast<int>(replica->sync_state));
            send_replication_msg(replica->name, command);
        }
    }
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

    repl_buffer_.erase(repl_buffer_.begin(), repl_buffer_.begin() + static_cast<long>(keep_count));
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

    // 序列化所有数据并通过 cluster bus 发送
    auto& storage = GlobalStorage::instance();
    auto all_kvs = storage.get_all_objects();
    int64_t sent_count = 0;

    for (const auto& [key, obj] : all_kvs) {
        std::string value_str;
        if (obj.type() == ObjectType::STRING) {
            auto opt_val = obj.get_string();
            if (opt_val.has_value()) {
                value_str = opt_val.value();
            } else {
                continue;
            }
        } else {
            // 对非 STRING 类型使用 serialize() 编码
            value_str = obj.serialize();
        }
        std::string cmd_line = "SET " + key + " " + value_str;
        if (!send_replication_msg(replica_name, cmd_line)) {
            LOG_WARN(CLUSTER, "Failed to send RDB entry for key=%s to replica=%s",
                     key.c_str(), replica_name.c_str());
        }
        sent_count++;
    }

    // 标记 RDB 发送完成，进入增量同步状态
    replica->sync_state = SyncState::kConnected;

    rdb_send_in_progress_.store(false);

    LOG_INFO(CLUSTER, "RDB sent to replica %s: %ld keys", replica_name.c_str(), sent_count);
    return true;
}

bool ReplicationMgr::send_replication_msg(const std::string& replica_name,
                                           const std::string& cmd_line) {
    auto* conn = ClusterServer::instance().getConnection();
    if (!conn) {
        return false;
    }

    // 通过 ClusterConnection 发送 cluster 消息，args[0]=cmd_line
    std::vector<std::string> args = {cmd_line};
    return conn->send_command_to_node(replica_name, args);
}

void ReplicationMgr::handle_replication_command(const std::string& cmd_line) {
    // 使用 CommandFactory 管道执行复制命令，不再手工解析
    static std::atomic<int64_t> repl_seq{0};
    int64_t seq = repl_seq.fetch_add(1);

    // 按空格分割命令行
    std::vector<std::string> args;
    {
        std::istringstream iss(cmd_line);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
    }

    if (args.empty()) {
        LOG_WARN(CLUSTER, "REPL-CMD[%ld] empty command line", seq);
        return;
    }

    std::string cmd_name = args[0];
    for (auto& c : cmd_name) c = static_cast<char>(std::tolower(c));

    LOG_INFO(CLUSTER, "REPL-CMD[%ld] cmd=%s key=%s",
             seq, cmd_name.c_str(),
             args.size() >= 2 ? args[1].c_str() : "-");

    auto command = CommandFactory::instance().create(cmd_name);
    if (command) {
        command->execute(args);
    } else {
        LOG_WARN(CLUSTER, "REPL-CMD[%ld] unknown command: %s", seq, cmd_name.c_str());
    }
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