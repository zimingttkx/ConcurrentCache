// cluster_connection.h
#ifndef CONCURRENTCACHE_CLUSTER_CONNECTION_H
#define CONCURRENTCACHE_CLUSTER_CONNECTION_H

#include "cluster_link.h"
#include "cluster_state.h"
#include "cluster_gossip.h"
#include "../network/event_loop.h"
#include "../network/channel.h"
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <functional>
#include <chrono>

namespace cc_server {

// ClusterConnection 类：管理所有集群节点间的连接
class ClusterConnection {
public:
    using NodeCallback = std::function<void(const std::string& node_name)>;
    using MsgCallback = std::function<void(const std::string& node_name, const ClusterMsg& msg)>;

    ClusterConnection();
    ~ClusterConnection();

    // 初始化
    void init();

    // 设置 EventLoop（用于注册 ClusterLink 的 socket fd）
    void set_event_loop(EventLoop* loop) { event_loop_ = loop; }

    // 启动/停止心跳
    void start_heartbeat();
    void stop_heartbeat();

    // 连接管理
    bool connect_to_node(const std::string& node_name, const std::string& ip, int port);
    void disconnect_from_node(const std::string& node_name);
    void disconnect_all();

    // 获取连接
    ClusterLink* get_link(const std::string& node_name);
    std::vector<ClusterLink*> get_all_links();

    // 向节点发送消息
    bool send_to_node(const std::string& node_name, const ClusterMsg& msg);
    bool ping_node(const std::string& node_name);
    bool pong_node(const std::string& node_name);
    bool meet_node(const std::string& node_name, const std::string& my_ip, int my_port);

    // 向节点发送 RESP 命令（用于 MIGRATE 等场景）
    bool send_command_to_node(const std::string& node_name, const std::vector<std::string>& args);

    // 广播消息
    void broadcast_ping();
    void broadcast_pong();
    void broadcast_gossip(const GossipMsg& msg);

    // 连接状态检查
    [[nodiscard]] size_t connected_count() const;
    [[nodiscard]] bool is_node_connected(const std::string& node_name) const;

    // 回调设置
    void set_node_connected_callback(NodeCallback cb) { node_connected_callback_ = std::move(cb); }
    void set_node_disconnected_callback(NodeCallback cb) { node_disconnected_callback_ = std::move(cb); }
    void set_msg_callback(ClusterLink::MsgCallback cb) { msg_callback_ = std::move(cb); }
    void set_gossip_callback(MsgCallback cb) { gossip_callback_ = std::move(cb); }
    void set_meet_callback(std::function<void(const std::string& ip, int port)> cb) { meet_callback_ = std::move(cb); }

    // 设置 ClusterState 引用（用于获取本节点信息）
    void set_state(ClusterState* state) { state_ = state; }

    // 心跳配置
    void set_heartbeat_interval(int64_t ms) { heartbeat_interval_ms_ = ms; }
    void set_ping_timeout(int64_t ms) { ping_timeout_ms_ = ms; }

    // 定时任务（供外部调用）
    void on_timer();

    // 注册/注销 ClusterLink 的 fd 到 EventLoop
    void register_link_to_loop(ClusterLink* link);
    void unregister_link_from_loop(ClusterLink* link);

private:
    // 定时任务
    void check_connections();

    // 节点断开处理
    void on_node_disconnected(const std::string& node_name, ClusterLink* link);

    // 处理收到的消息
    void handle_link_msg(ClusterMsg&& msg, ClusterLink* link);

    ClusterState* state_ = nullptr;  // 集群状态
    EventLoop* event_loop_ = nullptr;  // EventLoop 指针

    std::unordered_map<std::string, std::unique_ptr<ClusterLink>> links_;  // 节点连接
    mutable std::shared_mutex links_mutex_;  // 保护 links_

    // ClusterLink fd 到 Channel 的映射（用于 EventLoop 注销）
    std::unordered_map<int, Channel*> link_channels_;
    std::mutex channel_mutex_;  // 保护 link_channels_

    NodeCallback node_connected_callback_;
    NodeCallback node_disconnected_callback_;
    ClusterLink::MsgCallback msg_callback_;
    MsgCallback gossip_callback_;
    NodeCallback ping_timeout_callback_;
    std::function<void(const std::string& ip, int port)> meet_callback_;

    // 心跳相关
    int64_t heartbeat_interval_ms_ = 1000;      // 心跳间隔（毫秒）
    int64_t ping_timeout_ms_ = 5000;             // PING 超时时间（毫秒）
    int64_t last_heartbeat_time_ms_ = 0;        // 上次心跳时间
    bool heartbeat_running_ = false;             // 心跳是否运行中
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_CONNECTION_H