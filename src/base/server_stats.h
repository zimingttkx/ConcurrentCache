// server_stats.h — 进程级、跨线程的少量累计计数器。
//
// 为什么要有这个文件：INFO 的 total_connections_received / total_commands_processed
// 历史上是写死的 0，而它们是运维判断"连接泄漏 / 流量异常"的第一入口。写死的字段
// 比缺失的字段危险：它看起来像数据。
//
// header-only 是因为这几行不值得为它动 CMakeLists 的源文件清单（那会让新增文件
// 必须同时改两处构建表，多一个漏改的机会）。
#ifndef CONCURRENTCACHE_BASE_SERVER_STATS_H
#define CONCURRENTCACHE_BASE_SERVER_STATS_H

#include <atomic>
#include <cstdint>

namespace cc_server {

class ServerStats {
public:
    static ServerStats& instance() {
        static ServerStats s;
        return s;
    }

    ServerStats(const ServerStats&) = delete;
    ServerStats& operator=(const ServerStats&) = delete;

    // .relaxed：这些是观测用的单调计数，不参与任何同步决策，不需要顺序保证。
    void record_connection_accepted() {
        connections_accepted_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_command_processed() {
        commands_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t connections_accepted() const {
        return connections_accepted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t commands_processed() const {
        return commands_processed_.load(std::memory_order_relaxed);
    }

private:
    ServerStats() = default;

    std::atomic<uint64_t> connections_accepted_{0};
    std::atomic<uint64_t> commands_processed_{0};
};

}  // namespace cc_server

#endif
