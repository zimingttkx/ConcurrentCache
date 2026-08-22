//
// Created by Administrator on 2026/5/5.
//
#include "cache/expiration_checker.h"
#include "cache/expire_dict.h"
#include "cache/storage.h"
#include "base/log.h"
#include <cassert>
#include <chrono>
#include <thread>

namespace cc_server {
    ExpirationChecker::ExpirationChecker(ExpireDict& expire_dict, GlobalStorage& storage) :
        expire_dict_(expire_dict), storage_(storage), running_(false) {
        // 防御性检查：确保引用有效
        assert(&expire_dict_ != nullptr && "ExpirationChecker - expire_dict reference is null");
        assert(&storage_ != nullptr && "ExpirationChecker - storage reference is null");
        LOG_DEBUG(EXPIRE, "ExpirationChecker created");
    }

    ExpirationChecker::~ExpirationChecker() {
        stop();
        LOG_DEBUG(EXPIRE, "ExpirationChecker destroyed");
    }

    void ExpirationChecker::start() {
        if (running_.load()) {
            LOG_WARN(EXPIRE, "ExpirationChecker already running, ignoring start()");
            return;
        }

        // 确保之前的线程已结束
        if (thread_.joinable()) {
            LOG_WARN(EXPIRE, "ExpirationChecker - thread still joinable, joining before restart");
            thread_.join();
        }

        running_ = true;
        thread_ = std::thread(&ExpirationChecker::run, this);
        LOG_INFO(EXPIRE, "ExpirationChecker started");
    }

    void ExpirationChecker::stop() {
        if (!running_.load()) {
            LOG_DEBUG(EXPIRE, "ExpirationChecker already stopped, ignoring stop()");
            return;
        }
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
            LOG_INFO(EXPIRE, "ExpirationChecker stopped");
        } else {
            LOG_WARN(EXPIRE, "ExpirationChecker - thread not joinable after stop");
        }
    }

    void ExpirationChecker::run() {
        LOG_INFO(EXPIRE, "ExpirationChecker run loop started, thread_id=%zu",
                std::hash<std::thread::id>{}(std::this_thread::get_id()));

        while (running_.load()) {
            size_t deleted_count = 0;
            size_t checked_count = 0;

            // 时间预算内反复抽样删除（修复 P1-7：此前每轮只取 20 个候选，
            // 大规模过期键会长时间驻留内存）。Redis 的 activeExpireCycle 行为。
            auto cycle_start = std::chrono::steady_clock::now();
            bool budget_exhausted = false;

            try {
                do {
                    if (!running_.load()) break;

                    auto candidates = expire_dict_.get_candidates(20);
                    checked_count += candidates.size();
                    if (candidates.empty()) break;

                    for (const auto& key : candidates) {
                        if (!running_.load()) break;
                        if (expire_dict_.is_expired(key)) {
                            bool deleted = storage_.del(key);
                            if (deleted) ++deleted_count;
                        }
                    }

                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - cycle_start).count();
                    if (elapsed >= kMaxCheckDurationMs) {
                        budget_exhausted = true;
                    }
                } while (!budget_exhausted);
            } catch (const std::exception& e) {
                LOG_ERROR(EXPIRE, "Exception in expiration check loop: %s", e.what());
            } catch (...) {
                LOG_ERROR(EXPIRE, "Unknown exception in expiration check loop");
            }

            if (deleted_count > 0) {
                LOG_INFO(EXPIRE, "Periodic cleanup: checked=%zu, deleted=%zu",
                        checked_count, deleted_count);
            }

            // 在预算用尽或候选耗尽后休眠一个检查间隔
            std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
        }

        LOG_INFO(EXPIRE, "ExpirationChecker run loop exited");
    }

}
