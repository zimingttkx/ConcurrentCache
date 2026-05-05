//
// Created by Administrator on 2026/5/5.
//
#include "expire_dict.h"
#include "base/log.h"
#include <cassert>
#include <ranges>

namespace cc_server {
    ExpireDict::ExpireDict() = default;

    int64_t ExpireDict::current_time_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void ExpireDict::set(const std::string &key, int64_t expire_ms) {
        // 参数校验：过期时间必须大于0
        assert(expire_ms > 0 && "ExpireDict::set - expire_ms must be positive");

        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int64_t expire_time = current_time_ms() + expire_ms;
        expire_map_[key] = expire_time;

        LOG_DEBUG(EXPIRE, "Set expire for key=%s, expire_at=%ld, ttl_ms=%ld",
                 key.c_str(), expire_time, expire_ms);
    }

    int64_t ExpireDict::get_ttl(const std::string &key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return -2; // 键不存在
        }
        int64_t remaining = it->second - current_time_ms();
        if (remaining <= 0) {
            return -2;
        }
        return remaining;
    }

    int64_t ExpireDict::get_expire_time(const std::string &key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return -1; // 不存在 视为永不过期
        }
        return it->second;
    }

    bool ExpireDict::persist(const std::string &key) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            LOG_DEBUG(EXPIRE, "Persist failed - key not found: %s", key.c_str());
            return false; // 键不存在
        }
        expire_map_.erase(it); // 移除过期记录
        LOG_DEBUG(EXPIRE, "Persist success - key now permanent: %s", key.c_str());
        return true;
    }

    void ExpireDict::remove(const std::string &key) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        size_t erased = expire_map_.erase(key);
        if (erased > 0) {
            LOG_DEBUG(EXPIRE, "Removed expire record for key=%s", key.c_str());
        }
    }

    bool ExpireDict::is_expired(const std::string &key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return false; // 不存在 视为永不过期
        }
        bool expired = current_time_ms() >= it->second;
        if (expired) {
            LOG_DEBUG(EXPIRE, "Key expired: %s, expire_at=%ld",
                     key.c_str(), it->second);
        }
        return expired;
    }

    bool ExpireDict::contains(const std::string &key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return expire_map_.contains(key);
    }

    std::vector<std::string> ExpireDict::get_candidates(int n) const {
        // 参数校验
        assert(n > 0 && "ExpireDict::get_candidates - n must be positive");

        std::shared_lock<std::shared_mutex> lock(mutex_);

        // 收集所有键
        std::vector<std::string> all_keys;
        all_keys.reserve(expire_map_.size());
        for (const auto& [key, _] : expire_map_) {
            all_keys.push_back(key);
        }

        // 随机打乱
        std::shuffle(all_keys.begin(), all_keys.end(),
                     std::mt19937{std::random_device{}()});

        // 取前 n 个作为候选
        std::vector<std::string> candidates;
        candidates.reserve(n);
        for (int i = 0; i < n && i < static_cast<int>(all_keys.size()); ++i) {
            candidates.push_back(all_keys[i]);
        }

        LOG_TRACE(EXPIRE, "Got %zu candidates (requested %d), total keys=%zu",
                 candidates.size(), n, expire_map_.size());

        return candidates;
    }

    size_t ExpireDict::delete_expired() {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        size_t deleted_count = 0;
        int64_t now = current_time_ms();

        for (auto it = expire_map_.begin(); it != expire_map_.end();) {
            if (now >= it->second) { // 已过期
                it = expire_map_.erase(it);
                ++deleted_count;
            } else {
                ++it;
            }
        }

        if (deleted_count > 0) {
            LOG_INFO(EXPIRE, "Deleted %zu expired keys, remaining=%zu",
                    deleted_count, expire_map_.size());
        }

        return deleted_count;
    }

    size_t ExpireDict::size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return expire_map_.size();
    }

    void ExpireDict::clear_all() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const size_t cleared = expire_map_.size();
        expire_map_.clear();
        LOG_INFO(EXPIRE, "Cleared all expire records, cleared_count=%zu", cleared);
    }

}
