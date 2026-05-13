// cluster_server.h
#ifndef CONCURRENTCACHE_CLUSTER_SERVER_H
#define CONCURRENTCACHE_CLUSTER_SERVER_H

#include "cluster_state.h"
#include "cluster_connection.h"
#include "cluster_gossip.h"
#include <atomic>
#include <memory>
#include <string>

namespace cc_server {

class ClusterServer {
public:
    // 获取单例实例
    [[nodiscard]] static ClusterServer& instance();

    // 初始化（读取配置，创建本节点）
    void init();

    // 启动/停止
    void start();
    void stop();

    // 集群是否启用
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    // 集群是否运行中
    [[nodiscard]] bool isRunning() const { return running_.load(); }

    // 获取本节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getMyNode() { return my_node_; }

    // 获取集群状态
    [[nodiscard]] ClusterState* getState() { return &state_; }

    // 获取连接管理器
    [[nodiscard]] ClusterConnection* getConnection() { return &connection_; }

    // 获取 Gossip 协议
    [[nodiscard]] ClusterGossip* getGossip() { return &gossip_; }

    // 设置 EventLoop（需要在 EventLoop 创建后调用）
    void set_event_loop(EventLoop* loop) { connection_.set_event_loop(loop); }

    // 定时器回调（供 EventLoop 调用）
    void on_timer();

    // 槽相关
    // 将 key 映射到槽号（0-16383）
    [[nodiscard]] int keyToSlot(const std::string& key) const;

    // 根据 key 获取负责该 key 的节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeByKey(const std::string& key) const;

    // 根据槽号获取负责该槽的节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeBySlot(int slot) const;

    // 获取本节点名称
    [[nodiscard]] std::string getMyNodeName() const;

    // 槽迁移状态管理
    void setSlotMigrating(int slot, const std::string& target_node);   // 标记槽正在迁出到目标节点
    void setSlotImporting(int slot, const std::string& source_node);    // 标记槽正在从源节点迁入
    void clearSlotMigration(int slot);                                  // 清除槽迁移状态
    void setSlotOwner(int slot, const std::string& node_name);          // 设置槽的归属节点
    [[nodiscard]] bool isSlotMigrating(int slot) const;                // 槽是否正在迁出
    [[nodiscard]] bool isSlotImporting(int slot) const;                // 槽是否正在迁入
    [[nodiscard]] SlotMigrationInfo getSlotMigrationInfo(int slot) const;  // 获取槽迁移信息
    [[nodiscard]] std::shared_ptr<ClusterNode> getSlotMigrationTarget(int slot) const;  // 获取槽迁移目标节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getSlotMigrationSource(int slot) const;  // 获取槽迁移源节点

    // 请求路由检查
    // 检查是否需要对请求进行 ASK/MOVED 重定向
    // 返回空字符串表示不需要重定向，可以执行命令
    // 返回非空表示需要重定向，内容是 RESP 格式的重定向响应
    [[nodiscard]] std::string checkRedirect(const std::string& key) const;

    // 主从复制管理
    // 设置本节点为指定主节点的从节点
    bool setReplicaOf(const std::string& master_name);
    // 取消复制关系，本节点变为主节点
    void clearReplicaOf();
    // 检查本节点是否是从节点
    [[nodiscard]] bool isReplica() const;
    // 获取本节点的主节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getMyMaster() const;
    // 获取本节点的所有从节点
    [[nodiscard]] std::vector<std::shared_ptr<ClusterNode>> getMyReplicas() const;

    // 故障检测管理
    // 处理节点超时（被其他节点报告超时）
    void handleNodeTimeout(const std::string& node_name);
    // 处理节点恢复（收到节点的心跳）
    void handleNodeRecovery(const std::string& node_name);
    // 检查是否达到客观下线条件
    bool checkFailQuorum(const std::string& node_name);
    // 手动标记节点为 FAIL
    void markNodeAsFail(const std::string& node_name);

    // 故障转移管理
    // 发起故障转移（当从节点检测到主节点FAIL时调用）
    bool startFailover(const std::string& master_name);
    // 执行故障转移（定时器调用）
    void executeFailover();
    // 检查是否应该发起故障转移
    bool shouldStartFailover();
    // 处理投票请求
    bool handleFailoverAuthRequest(const std::string& replica_name, int64_t epoch, int64_t offset);
    // 处理投票响应
    void handleFailoverAuthAck(const std::string& node_name);
    // 完成故障转移
    void completeFailover();
    // 获取需要故障转移的主节点的从节点列表
    std::vector<std::shared_ptr<ClusterNode>> getReplicasForMaster(const std::string& master_name) const;

    // 禁用拷贝
    ClusterServer(const ClusterServer&) = delete;
    ClusterServer& operator=(const ClusterServer&) = delete;

private:
    // 私有构造函数
    ClusterServer() = default;
    ~ClusterServer() = default;

    bool enabled_ = false;                          // 是否启用集群模式
    std::shared_ptr<ClusterNode> my_node_;         // 本节点
    ClusterState state_;                            // 集群状态
    ClusterConnection connection_;                   // 连接管理器
    ClusterGossip gossip_;                          // Gossip 协议
    std::atomic<bool> running_{false};             // 运行状态
};

} // namespace cc_server

#endif
