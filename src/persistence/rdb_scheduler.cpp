//
// RDB 自动保存调度器实现
//

#include "persistence/rdb_scheduler.h"
#include "persistence/rdb.h"
#include "cache/storage.h"
#include "base/log.h"
#include <thread>
#include <chrono>

namespace cc_server {

static constexpr const char* kSchedulerModule = "RDB_SCHEDULER";

RdbScheduler::RdbScheduler(GlobalStorage& storage, const std::string& rdb_path)
    : storage_(storage), rdb_path_(rdb_path) {
}

RdbScheduler::~RdbScheduler() {
    stop();
}

void RdbScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        LOG_WARN(kSchedulerModule, "Scheduler already running");
        return;
    }

    running_.store(true, std::memory_order_release);
    scheduler_thread_ = std::thread(&RdbScheduler::schedule_loop, this);

    LOG_INFO(kSchedulerModule, "RDB Scheduler started, interval=%ds, dirty_threshold=%d",
             config_.interval_sec, config_.dirty_threshold);
}

void RdbScheduler::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);

    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }

    LOG_INFO(kSchedulerModule, "RDB Scheduler stopped");
}

void RdbScheduler::schedule_loop() {
    auto last_save_time = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        // 每秒检查一次
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 获取当前脏键数量
        size_t current_dirty = storage_.get_dirty_count();

        // 计算距离上次保存的时间
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_save_time).count();

        // 检查是否需要保存：
        // 1. 超过配置的间隔时间
        // 2. 脏键数量增加超过阈值
        bool should_save = false;

        if (elapsed >= config_.interval_sec) {
            should_save = true;
            LOG_DEBUG(kSchedulerModule, "Trigger save: interval elapsed (%ld seconds)", elapsed);
        } else if (current_dirty >= static_cast<size_t>(config_.dirty_threshold)) {
            // 脏键数量达到阈值，触发保存
            should_save = true;
            LOG_DEBUG(kSchedulerModule, "Trigger save: dirty threshold reached (%zu >= %zu)",
                     current_dirty, config_.dirty_threshold);
        }

        if (should_save) {
            do_save();
            last_save_time = std::chrono::steady_clock::now();
        }
    }
}

void RdbScheduler::do_save() {
    auto& rdb = RdbPersistence::instance();
    rdb.set_filepath(rdb_path_);

    LOG_INFO(kSchedulerModule, "Auto-save triggered...");

    // 记录保存前的脏计数快照
    size_t dirty_snapshot = storage_.get_dirty_count();

    // 使用异步保存避免阻塞调度线程
    if (rdb.save_in_background(rdb_path_, storage_)) {
        LOG_INFO(kSchedulerModule, "Background save started, dirty_count=%zu", dirty_snapshot);
        // 只扣掉这次快照已经排进去的那部分（Redis 的 dirty_before_save 同理）。
        // 旧代码在这里 reset 成 0：快照开始之后、文件写完之前落进来的那批写，
        // 计数被抹掉，于是它们永远攒不到下一次触发——dirty_threshold 配得越小，
        // 丢得越彻底（配 1 时等于每次 BGSAVE 都把窗口期的写静默吞掉）。
        // 也不能等 BGSAVE 结束再扣：调度线程不阻塞，期间还会继续写。
        storage_.consume_dirty_count(dirty_snapshot);
    } else {
        LOG_ERROR(kSchedulerModule, "Failed to start background save");
    }
}

}  // namespace cc_server