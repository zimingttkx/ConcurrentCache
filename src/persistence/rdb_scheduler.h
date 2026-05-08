//
// RDB 自动保存调度器
//

#ifndef CONCURRENTCACHE_RDB_SCHEDULER_H
#define CONCURRENTCACHE_RDB_SCHEDULER_H

#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

namespace cc_server {

class GlobalStorage;

class RdbScheduler {
public:
    // 保存策略配置
    struct SaveConfig {
        int interval_sec = 900;      // 保存间隔（秒）
        int dirty_threshold = 1;     // 脏键阈值（达到即保存）
    };

    RdbScheduler(GlobalStorage& storage, const std::string& rdb_path);
    ~RdbScheduler();

    // 禁用拷贝
    RdbScheduler(const RdbScheduler&) = delete;
    RdbScheduler& operator=(const RdbScheduler&) = delete;

    /**
     * @brief 启动调度器
     */
    void start();

    /**
     * @brief 停止调度器
     */
    void stop();

    /**
     * @brief 检查是否正在运行
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    /**
     * @brief 设置保存配置
     */
    void set_config(const SaveConfig& config) { config_ = config; }

    /**
     * @brief 获取保存配置
     */
    const SaveConfig& get_config() const { return config_; }

private:
    // 调度线程主循环
    void schedule_loop();

    // 执行保存
    void do_save();

    GlobalStorage& storage_;
    std::string rdb_path_;
    std::atomic<bool> running_{false};
    std::thread scheduler_thread_;
    SaveConfig config_{900, 1};  // 默认 15 分钟或 1 个脏键
};

}  // namespace cc_server

#endif  // CONCURRENTCACHE_RDB_SCHEDULER_H
