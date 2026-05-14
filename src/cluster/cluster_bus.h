// cluster_bus.h
#ifndef CONCURRENTCACHE_CLUSTER_BUS_H
#define CONCURRENTCACHE_CLUSTER_BUS_H

#include "cluster_link.h"
#include "../network/event_loop.h"
#include "../network/channel.h"
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

    // Register/unregister link to EventLoop
    void register_link_to_loop(ClusterLink* link);
    void unregister_link_from_loop(ClusterLink* link);
    void unregister_link_from_loop_fd(int fd);  // 通过 fd 注销（用于避免 UAF）

    EventLoop* event_loop_ = nullptr;           // EventLoop pointer
    int listen_fd_ = -1;                        // Listen socket fd
    int listen_port_ = 0;                        // Actual listening port
    std::atomic<bool> running_{false};          // Running state

    Channel* listen_channel_ = nullptr;         // Listen socket's Channel

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