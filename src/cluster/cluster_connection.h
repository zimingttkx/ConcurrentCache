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

/// @brief 总线报文的两个平面，身份口径不同（理由见 bus_sender_identity_accepted）
enum class BusMsgPlane {
    kControl,  // PING / PONG / MEET 等成员状态交换
    kData,     // kRepData：被复制的写命令与 REPLSYNC
};

/**
 * @brief 这条总线报文可不可以按"已认识成员"处理
 *
 * 为什么必须有这一步：`ClusterMsgHeader::sender_name`（"ip:port"）不只是日志字段，
 * 它是节点在集群里的**身份 key**——`ClusterState` 的表以它为键，PFAIL/FAIL 报告按它
 * 记名，`checkFailQuorum` 的法定人数按"有多少个不同的上报者提到这个键"来数。而这个端口
 * 不做任何认证，报文字段完全由发送方自报。于是一个只要能连上总线端口的进程就可以
 * 声称自己是第三个主节点，凭空把 FAIL 判定凑成法定人数，让一个活得好好的主节点被判死、
 * 触发副本升主；也可以声称是本端自己，往本端的状态表里写东西。
 *
 * 但"声称的 ip 必须与链路地址逐字相等"这条口径（#45 原来的写法）代价被低估了：
 * 本仓库把被复制的写命令也塞在同一条总线上，所以它不只拦伪造的成员判定，还连带拦掉
 * 复制数据面；而 hostname / Docker service-name / NAT 之后的部署里"对端自己是谁"与
 * "这条链路是什么地址"本来就永远不等（运维敲进 CLUSTER MEET 的是名字，入站 accept 的
 * 是 IP），于是报文被静默丢弃、对端等不到 PONG 就把本端标 PFAIL。把整条数据面绑死在
 * 一个字符串全等上，比它要防的问题更严重。
 *
 * 所以分平面处理：
 *  - 控制面：两侧都是字面地址（IPv4/IPv6 形状）时仍要求逐字相等——这正是 #45 要防的
 *    "冒充另一个成员"；任一侧是主机名时不做地址比对，否则 MEET 用主机名拨号这一步
 *    永远通不过。
 *  - 数据面：地址不作为凭据，要求的是"这个名字已经在我们表里"。握手期不会走数据面，
 *    所以不依赖先认识；而随机来客拿不出一个已在表里的节点名，仍然被拒。
 *
 * 这不是授权机制——它仍是启发式。总线自身的真实性目前没有解：Redis 的答案是
 * `tls-cluster yes`（mTLS，详见 SECURITY.md）；报文签名既不是 Redis 的做法、也
 * 还没做。等总线有了凭据，这两个平面也就不用再靠地址形状互相猜了。
 * 两侧任何一边为空一律判不通过：观察不到来源不构成免检理由。
 *
 * @param claimed_ip 报文 sender_name 里声称的地址（冒号前的部分）
 * @param observed_ip 本端实际看到的那条链路的对端地址
 * @param sender_is_known_member sender_name 整串是否已在 ClusterState 的节点表里
 * @param plane 这条报文属于控制面还是数据面
 */
[[nodiscard]] bool bus_sender_identity_accepted(const std::string& claimed_ip,
                                                const std::string& observed_ip,
                                                bool sender_is_known_member,
                                                BusMsgPlane plane);


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

    /// @brief 锁内只抄一份 shared_ptr，发送在锁外做（避免共享锁里回调再要独占锁）
    [[nodiscard]] std::vector<std::shared_ptr<ClusterLink>> snapshot_links() const;

    /// @brief 按节点名取一条链路的 shared_ptr 副本；找不到返回空
    [[nodiscard]] std::shared_ptr<ClusterLink> find_link(const std::string& node_name) const;

private:
    // 定时任务
    void check_connections();

    // 节点断开处理
    void on_node_disconnected(const std::string& node_name, ClusterLink* link);

    // 处理收到的消息
    void handle_link_msg(ClusterMsg&& msg, ClusterLink* link);

    ClusterState* state_ = nullptr;  // 集群状态
    EventLoop* event_loop_ = nullptr;  // EventLoop 指针

    // 出站链路的所有权用 shared_ptr：广播/发送这类路径必须**在锁外**调用
    // ClusterLink::send_*（见 .cpp 里 snapshot_links 的注释），而锁外持有的裸指针
    // 可能在对端擦除表项时被销毁。持有 shared_ptr 副本才能让对象活到自己发完。
    std::unordered_map<std::string, std::shared_ptr<ClusterLink>> links_;  // 节点连接
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