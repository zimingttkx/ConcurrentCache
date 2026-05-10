// cluster_server.h
#ifndef CONCURRENTCACHE_CLUSTER_SERVER_H
#define CONCURRENTCACHE_CLUSTER_SERVER_H

#include "cluster_state.h"
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

    // 槽相关
    // 将 key 映射到槽号（0-16383）
    [[nodiscard]] int keyToSlot(const std::string& key) const;

    // 根据 key 获取负责该 key 的节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeByKey(const std::string& key) const;

    // 根据槽号获取负责该槽的节点
    [[nodiscard]] std::shared_ptr<ClusterNode> getNodeBySlot(int slot) const;

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
    std::atomic<bool> running_{false};             // 运行状态
};

} // namespace cc_server

#endif
