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
#include <thread>
#include <atomic>

namespace cc_server {

/**
 * @brief gossip 报文的发送者身份是否与本端实际观察到的链路地址一致
 *
 * 为什么必须有这一步：`ClusterMsgHeader::sender_name`（"ip:port"）不只是日志字段，
 * 它是节点在集群里的**身份 key**——`ClusterState` 的表以它为键，PFAIL/FAIL 报告按它
 * 记名，`checkFailQuorum` 的法定人数按"有多少个不同的上报者提到这个键"来数。而这个端口
 * 不做任何认证，报文字段完全由发送方自报。于是一个只要能连上总线端口的进程就可以
 * 声称自己是第三个主节点，凭空把 FAIL 判定凑成法定人数，让一个活得好好的主节点被判死、
 * 触发副本升主；也可以声称是本端自己，往本端的状态表里写东西。
 *
 * 本端唯一能独立确定的事实是链路地址：入站链路是 accept() 看到的源 IP，出站链路是我们
 * 按 CLUSTER MEET 拨出去的那个 IP。报文里声称的 ip 必须与它一致。
 *
 * 这是一条比 Redis 更严的口径：Redis 允许 cluster-announce-ip 与源地址不同（NAT 后
 * 的多主机场景），它靠总线签名（cluster-secret 的 HMAC）弥补。本仓库没有签名机制，
 * 所以在跨 NAT 的部署里这条会拒收对端报文——那是一个明确的、可诊断的功能限制，比
 * "谁都能伪造集群成员判定"要安全。两侧都为空的字段一律判不通过：观察不到来源
 * 不构成免检理由。
 *
 * @param claimed_ip 报文 sender_name 里声称的 IP（冒号前的部分）
 * @param observed_ip 本端实际看到的那条链路的对端 IP
 */
[[nodiscard]] bool bus_sender_ip_matches_peer(const std::string& claimed_ip,
                                               const std::string& observed_ip);


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
    [[nodiscard]] EventLoop* get_event_loop() const { return event_loop_; }

    // 桥接方法（供 ClusterBus 调用，处理来自 cluster bus 端口的消息）
    void handle_incoming_msg(ClusterMsg&& msg, ClusterLink* link) {
        handle_link_msg(std::move(msg), link);
    }
    void handle_incoming_disconnect(const std::string& name, ClusterLink* link) {
        on_node_disconnected(name, link);
    }

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

    // 向节点发送原始字符串数据（用于复制命令推送）
    bool send_raw_to_node(const std::string& node_name, const std::string& data);

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
    void set_ping_timeout_callback(NodeCallback cb) { ping_timeout_callback_ = std::move(cb); }

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
    std::thread heartbeat_thread_;                // 心跳定时器线程
    std::atomic<bool> heartbeat_thread_stop_{false};  // 停止心跳线程标志
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_CONNECTION_H