// replication_mgr.h
#ifndef CONCURRENTCACHE_REPLICATION_MGR_H
#define CONCURRENTCACHE_REPLICATION_MGR_H

#include "cluster_node.h"
#include "cluster_state.h"
#include "../network/buffer.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <shared_mutex>
#include <atomic>
#include <cstdint>

namespace cc_server {

// 复制缓冲区元素
struct ReplicationBufferEntry {
    std::string command;  // RESP 格式的命令
    int64_t offset;       // 该命令的偏移量
};

// 复制状态
enum class SyncState {
    kNone = 0,           // 无同步
    kWaiting,            // 等待同步
    kSendingRdb,         // 正在发送 RDB
    kSendingkv,          // 正在发送增量命令
    kConnected           // 同步完成
};

// 副本信息
struct ReplicaInfo {
    std::string name;                      // 副本节点名称
    std::string ip;                         // 副本 IP
    int port;                               // 副本端口
    int64_t repl_offset;                    // 复制偏移量
    int64_t last_ack_time;                  // 最后确认时间
    SyncState sync_state;                   // 同步状态
    std::shared_ptr<ClusterNode> node;      // 副本节点指针

    ReplicaInfo() : port(0), repl_offset(0), last_ack_time(0),
                    sync_state(SyncState::kNone), node(nullptr) {}
};

// 复制管理器类
class ReplicationMgr {
public:
    ReplicationMgr();
    ~ReplicationMgr();

    // 禁用拷贝
    ReplicationMgr(const ReplicationMgr&) = delete;
    ReplicationMgr& operator=(const ReplicationMgr&) = delete;

    // 单例
    static ReplicationMgr& instance();

    // 初始化
    void init(ClusterState* state);

    // ========== 主节点操作 ==========

    // 添加一个副本连接
    void add_replica(const std::string& name, const std::string& ip, int port,
                     std::shared_ptr<ClusterNode> node);

    // 移除一个副本
    void remove_replica(const std::string& name);

    // 获取所有副本
    std::vector<std::shared_ptr<ReplicaInfo>> get_all_replicas() const;

    // 广播命令到所有副本（复制缓冲区）
    void replicate_command(const std::string& command);

    // 发送 RDB 文件给副本
    bool send_rdb_to_replica(const std::string& replica_name);

    // 通过 cluster bus 发送复制命令给指定副本
    bool send_replication_msg(const std::string& replica_name, const std::string& cmd_line);

    // 处理收到的复制命令（副本端调用）
    void handle_replication_command(const std::string& cmd_line);

    // 更新副本的确认偏移量
    void update_replica_ack_offset(const std::string& replica_name, int64_t offset);

    // 获取当前主节点的复制偏移量
    int64_t get_master_repl_offset() const { return master_repl_offset_.load(); }

    // ========== 副本节点操作 ==========

    // 设置主节点信息
    void set_master(const std::string& ip, int port, const std::string& master_runid);

    // 获取主节点信息
    std::string get_master_ip() const { return master_ip_; }
    int get_master_port() const { return master_port_; }
    std::string get_master_runid() const { return master_runid_; }

    // 更新主节点复制偏移量（副本收到后更新）
    void set_master_repl_offset(int64_t offset) { master_repl_offset_.store(offset); }

    // 获取复制状态
    SyncState get_sync_state() const { return sync_state_.load(); }
    void set_sync_state(SyncState state) { sync_state_.store(state); }

    // 获取主节点运行ID
    std::string get_master_runid() { return master_runid_; }

    // 生成一个随机的 runid
    static std::string generate_runid();

private:
    // 添加命令到复制缓冲区
    void add_to_replication_buffer(const std::string& command);

    // 获取复制的命令列表（从 offset 开始）
    std::vector<ReplicationBufferEntry> get_replication_commands(int64_t from_offset);

    // 清理过期的缓冲区数据
    void cleanup_replication_buffer();

    ClusterState* state_;                              // 集群状态指针
    mutable std::shared_mutex replicas_mutex_;          // 保护 replicas_

    // 主节点信息
    std::string master_ip_;                             // 主节点 IP
    int master_port_ = 0;                              // 主节点端口
    std::string master_runid_;                          // 主节点运行 ID
    std::atomic<int64_t> master_repl_offset_{0};       // 主节点复制偏移量

    // 副本列表
    std::unordered_map<std::string, std::shared_ptr<ReplicaInfo>> replicas_;

    // 复制缓冲区（环形缓冲区，存储最近的命令）
    static constexpr size_t kReplicationBufferSize = 10 * 1024 * 1024;  // 10MB
    std::vector<ReplicationBufferEntry> repl_buffer_;
    std::atomic<int64_t> repl_buffer_start_offset_{0};  // 缓冲区起始偏移量
    mutable std::mutex repl_buffer_mutex_;               // 保护 repl_buffer_

    // 同步状态
    std::atomic<SyncState> sync_state_{SyncState::kNone};

    // RDB 发送状态
    std::atomic<bool> rdb_send_in_progress_{false};
};

} // namespace cc_server

#endif // CONCURRENTCACHE_REPLICATION_MGR_H