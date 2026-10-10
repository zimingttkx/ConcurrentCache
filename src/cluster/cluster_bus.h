// cluster_bus.h
#ifndef CONCURRENTCACHE_CLUSTER_BUS_H
#define CONCURRENTCACHE_CLUSTER_BUS_H

#include "cluster_link.h"
#include "../network/event_loop.h"
#include "../network/channel.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

namespace cc_server {

// ClusterBus class: cluster inter-node communication bus
// Listens on server_port + 10000, handles all cluster node TCP connections
class ClusterBus {
public:
    ClusterBus();
    ~ClusterBus();

    ClusterBus(const ClusterBus&) = delete;
    ClusterBus& operator=(const ClusterBus&) = delete;

    // Initialize (set EventLoop)
    void init(EventLoop* loop);

    // Start/stop cluster bus
    bool start(int server_port);
    void stop();

    // Set callbacks
    void set_msg_callback(ClusterLink::MsgCallback cb) { msg_callback_ = std::move(cb); }
    void set_disconnect_callback(ClusterLink::DisconnectCallback cb) { disconnect_callback_ = std::move(cb); }

    // Get listening port
    [[nodiscard]] int port() const { return listen_port_; }

    // Is running
    [[nodiscard]] bool is_running() const { return running_.load(); }

private:
    // Handle listen socket readable event (accept new connections)
    void handle_accept();

    // Create new ClusterLink (for inbound connections)
    ClusterLink* create_link(int fd, const std::string& node_name, const std::string& ip, int port);

    // Remove link
    void remove_link(const std::string& node_name);

public:
    // 当前持有的入站链路数（每条 = 一个 fd + 一个 ClusterLink + 一个 Channel +
    // 收发缓冲）。集群规模是几十的量级，配额给到 512 已经远超需要；上限存在的
    // 理由不是"防大集群"，而是总线端口不做认证（见 tls-cluster 那条待拍板项），
    // 反复连进来的对端能把 fd 打到 EMFILE —— 那时 accept 返回 -1 又不是 EAGAIN，
    // handle_accept 会 break，而 listen fd 持续可读，EventLoop 于是 100% CPU 空转。
    static constexpr size_t kMaxInboundLinks = 512;

    // 纯谓词：单元测试可以直接钉它（把配额判断写成内联 if 就只能靠起真服务器来验）。
    static bool inbound_admitted(size_t current_links, size_t limit = kMaxInboundLinks) {
        return current_links < limit;
    }

    [[nodiscard]] size_t link_count() const;
    [[nodiscard]] uint64_t inbound_refused() const { return inbound_refused_.load(); }

private:

    // Register/unregister link to EventLoop
    void register_link_to_loop(ClusterLink* link);
    void unregister_link_from_loop(ClusterLink* link);
    void unregister_link_from_loop_fd(int fd);  // 通过 fd 注销（用于避免 UAF）
    // 把某条链路的 Channel 从 epoll 与 link_channels_ 摘掉但**不 delete**，交回指针
    // 给调用方处置。断开常常发生在 Channel::handle_event 的栈里，当场 delete 就是
    // 释放正在执行的那个对象，所以销毁时机必须由调用方决定（见 remove_link）。
    Channel* detach_link_channel(int fd);

    EventLoop* event_loop_ = nullptr;           // EventLoop pointer
    int listen_fd_ = -1;                        // Listen socket fd
    int listen_port_ = 0;                        // Actual listening port
    std::atomic<bool> running_{false};          // Running state

    Channel* listen_channel_ = nullptr;         // Listen socket's Channel

    std::atomic<uint64_t> inbound_refused_{0};  // 因超过配额而关掉的入站连接数

    std::unordered_map<std::string, std::unique_ptr<ClusterLink>> links_;  // All connections
    mutable std::shared_mutex links_mutex_;      // Protect links_

    // fd to Channel mapping (for unregister)
    std::unordered_map<int, Channel*> link_channels_;
    std::mutex channel_mutex_;                   // Protect link_channels_

    ClusterLink::MsgCallback msg_callback_;
    ClusterLink::DisconnectCallback disconnect_callback_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_CLUSTER_BUS_H