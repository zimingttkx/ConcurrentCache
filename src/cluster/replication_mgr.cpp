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

    // 修复 P1-2c：全量同步期间（kSendingRdb）的写入不能既不在快照里、
    // 又不推增量（旧代码直接跳过 → 永久丢失）。这些命令先记入该副本的
    // backlog，快照发送完成后按序回放（见 send_rdb_to_replica 尾部）。
    {
        std::unique_lock<std::shared_mutex> lock(replicas_mutex_);
        for (auto& [name, replica] : replicas_) {
            if (replica && replica->sync_state == SyncState::kSendingRdb) {
                replica->pending_commands.push_back(command);
            }
        }
    }

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
    auto all_kvs = storage.get_all_objects_with_ttl();
    int64_t sent_count = 0;

    // 修复 P1-2b：全量同步改用 RESTORE 传输完整 CacheObject（保留 LIST/HASH/
    // SET/ZSET 类型），并携带 TTL。旧代码对一切类型发 "SET key value_str"：
    // 非 STRING 类型经 serialize() 的多行文本在副本上被存成错误的 STRING，
    // 之后 LPUSH/HGET 等命令在副本上永远 WRONGTYPE，主从类型永久不一致。
    for (const auto& kv : all_kvs) {
        // args: RESTORE key ttl_ms serialized_value REPLACE
        // ttl_ms 取剩余生存时间；永不过期为 -1（与 RESTORE 命令语义一致）
        int64_t ttl_ms = -1;
        if (kv.expire_time_ms > 0) {
            ttl_ms = kv.expire_time_ms - storage.current_time_ms();
            if (ttl_ms <= 0) {
                continue;  // 快照时已过期，不发
            }
        }
        std::vector<std::string> args = {
            "RESTORE", kv.key, std::to_string(ttl_ms), kv.value.serialize(), "REPLACE"
        };
        if (!send_replication_args(replica_name, args)) {
            LOG_WARN(CLUSTER, "Failed to send RESTORE for key=%s to replica=%s",
                     kv.key.c_str(), replica_name.c_str());
        }
        sent_count++;
    }

    // 修复 P1-2c：快照发送完成，回放期间积压的写命令，然后进入增量同步。
    // 顺序：先取走 backlog 再切 kConnected —— 取走后新命令直接走增量推送，
    // 取走前的命令都在 backlog 快照里，不丢不重。
    std::vector<std::string> backlog;
    {
        std::unique_lock<std::shared_mutex> lock(replicas_mutex_);
        auto it = replicas_.find(replica_name);
        if (it != replicas_.end()) {
            backlog = std::move(it->second->pending_commands);
            it->second->pending_commands.clear();
            it->second->sync_state = SyncState::kConnected;
        }
    }
    for (const auto& cmd : backlog) {
        send_replication_msg(replica_name, cmd);
    }

    rdb_send_in_progress_.store(false);

    LOG_INFO(CLUSTER, "RDB sent to replica %s: %ld keys, %zu backlog commands replayed",
             replica_name.c_str(), sent_count, backlog.size());
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

bool ReplicationMgr::send_replication_args(const std::string& replica_name,
                                           const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }
    auto* conn = ClusterServer::instance().getConnection();
    if (!conn) {
        return false;
    }

    // 修复 P1-2a：把命令编码为 RESP 数组文本（二进制安全），整段作为单个
    // kRepData 参数传输。RESP bulk string 自带长度前缀，key/value 中的空格、
    // \xC0（bus 参数分隔符）、\n（serialize 多行文本）都不会再被截断。
    std::string resp_cmd = RespEncoder::encode_array(args);
    std::vector<std::string> bus_args = {resp_cmd};
    return conn->send_command_to_node(replica_name, bus_args);
}

void ReplicationMgr::handle_replication_command(const std::string& cmd_line) {
    // 使用 CommandFactory 管道执行复制命令，不再手工解析
    static std::atomic<int64_t> repl_seq{0};
    int64_t seq = repl_seq.fetch_add(1);

    // 修复 P1-2a：优先按 RESP 数组解析（新协议，二进制安全）。
    // 首字节是 '*' 且能完整解析 → RESP 数组；否则回退旧的空格分割
    // （兼容运行中升级窗口内旧主节点发来的文本命令）。
    std::vector<std::string> args;
    bool parsed = false;
    if (!cmd_line.empty() && cmd_line[0] == '*') {
        Buffer tmp;
        tmp.append(cmd_line.data(), cmd_line.size());
        RespParser parser;
        std::vector<RespValue> parsed_values = parser.parse(&tmp);
        if (!parser.error().empty()) {
            LOG_WARN(CLUSTER, "REPL-CMD[%ld] malformed RESP payload: %s",
                     seq, parser.error().c_str());
            return;
        }
        if (!parsed_values.empty() && parsed_values[0].type == RespType::ARRAY) {
            for (const auto& v : parsed_values[0].as_array()) {
                args.push_back(v.as_string());
            }
            parsed = true;
        }
    }
    if (!parsed) {
        // 旧格式回退：按空格分割命令行（不识别含空格的 value，仅兼容旧对端）
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