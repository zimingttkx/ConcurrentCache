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
        }
    }

    void ExpirationChecker::run() {
        LOG_INFO(EXPIRE, "ExpirationChecker run loop started, thread_id=%zu",
                std::hash<std::thread::id>{}(std::this_thread::get_id()));

        while (running_.load()) {
            size_t deleted_count = 0;

            try {
                // 获取候选键进行检查
                auto candidates = expire_dict_.get_candidates(20);
                for (const auto& key : candidates) {
                    // 再次检查运行状态，防止长时间操作阻塞退出
                    if (!running_.load()) {
                        break;
                    }

                    if (expire_dict_.is_expired(key)) {
                        // 同时删除 GlobalStorage 中的数据和 ExpireDict 中的记录
                        bool deleted = storage_.del(key);
                        if (deleted) {
                            ++deleted_count;
                            LOG_DEBUG(EXPIRE, "Periodic delete: key=%s", key.c_str());
                        }
                    }
                }

                if (deleted_count > 0) {
                    LOG_INFO(EXPIRE, "Periodic cleanup: deleted %zu expired keys", deleted_count);
                }

            } catch (const std::exception& e) {
                LOG_ERROR(EXPIRE, "Exception in expiration check loop: %s", e.what());
            } catch (...) {
                LOG_ERROR(EXPIRE, "Unknown exception in expiration check loop");
            }

            // 休眠
            std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
        }

        LOG_INFO(EXPIRE, "ExpirationChecker run loop exited");
    }

}
