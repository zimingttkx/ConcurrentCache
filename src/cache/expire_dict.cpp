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
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void ExpireDict::set(const std::string &key, int64_t expire_ms) {
        // 参数校验：过期时间必须大于0
        assert(expire_ms > 0 && "ExpireDict::set - expire_ms must be positive");
        assert(!key.empty() && "ExpireDict::set - key is empty");

        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int64_t expire_time = current_time_ms() + expire_ms;
        expire_map_[key] = expire_time;

        LOG_DEBUG(EXPIRE, "Set expire for key=%s, expire_at=%ld, ttl_ms=%ld",
                 key.c_str(), expire_time, expire_ms);
    }

    void ExpireDict::set_expire_time(const std::string &key, int64_t expire_time_ms) {
        assert(!key.empty() && "ExpireDict::set_expire_time - key is empty");
        assert(expire_time_ms > 0 && "ExpireDict::set_expire_time - expire_time_ms must be positive");

        std::unique_lock<std::shared_mutex> lock(mutex_);
        expire_map_[key] = expire_time_ms;

        LOG_DEBUG(EXPIRE, "Set expire time for key=%s, expire_at=%ld", key.c_str(), expire_time_ms);
    }

    int64_t ExpireDict::get_ttl(const std::string &key) const {
        assert(!key.empty() && "ExpireDict::get_ttl - key is empty");

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return -2; // 键不存在
        }
        int64_t remaining = it->second - current_time_ms();
        if (remaining <= 0) {
            return -2; // 已过期
        }
        return remaining;
    }

    int64_t ExpireDict::get_expire_time(const std::string &key) const {
        assert(!key.empty() && "ExpireDict::get_expire_time - key is empty");

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return -1; // 不存在 视为永不过期
        }
        return it->second;
    }

    bool ExpireDict::persist(const std::string &key) {
        assert(!key.empty() && "ExpireDict::persist - key is empty");

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            LOG_DEBUG(EXPIRE, "Persist failed - key not found: %s", key.c_str());
            return false; // 键不存在
        }
        int64_t expire_time = it->second;
        expire_map_.erase(it);
        LOG_INFO(EXPIRE, "Persist success - key now permanent: %s (was expire_at=%ld)",
                key.c_str(), expire_time);
        return true;
    }

    void ExpireDict::remove(const std::string &key) {
        assert(!key.empty() && "ExpireDict::remove - key is empty");

        std::unique_lock<std::shared_mutex> lock(mutex_);
        size_t erased = expire_map_.erase(key);
        if (erased > 0) {
            LOG_DEBUG(EXPIRE, "Removed expire record for key=%s", key.c_str());
        }
    }

    bool ExpireDict::is_expired(const std::string &key) const {
        assert(!key.empty() && "ExpireDict::is_expired - key is empty");

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = expire_map_.find(key);
        if (it == expire_map_.end()) {
            return false; // 不存在 视为永不过期
        }
        int64_t now = current_time_ms();
        bool expired = now >= it->second;
        if (expired) {
            LOG_DEBUG(EXPIRE, "Key expired: %s, expire_at=%ld, now=%ld",
                     key.c_str(), it->second, now);
        }
        return expired;
    }

    bool ExpireDict::contains(const std::string &key) const {
        assert(!key.empty() && "ExpireDict::contains - key is empty");

        std::shared_lock<std::shared_mutex> lock(mutex_);
        return expire_map_.contains(key);
    }

    std::vector<std::string> ExpireDict::get_candidates(int n) const {
        // 参数校验
        assert(n > 0 && "ExpireDict::get_candidates - n must be positive");

        std::shared_lock<std::shared_mutex> lock(mutex_);

        const size_t total = expire_map_.size();
        if (total == 0) {
            LOG_TRACE(EXPIRE, "Got 0 candidates (requested %d), no keys in expire_dict", n);
            return {};
        }

        // 修复 P2-perf：改为随机采样（Redis activeExpireCycle 的做法）。
        // 旧实现把整个 expire_map_ 拷贝成 vector 再 shuffle——10 万个带 TTL 的
        // key 时，每 100ms 深拷贝 10 万个 std::string 并长时间持共享锁，
        // 阻塞 EXPIRE/TTL 等写路径。现在只从迭代器随机定位 n 个位置。
        const size_t want = std::min(static_cast<size_t>(n), total);

        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<size_t> dist(0, total - 1);

        std::vector<std::string> candidates;
        candidates.reserve(want);

        // 随机取 want 个下标，用 std::advance 定位（无序 map 不支持随机访问）。
        // want 通常为 20，代价远小于全表拷贝；极端 n 接近 total 时（want*4>=total）
        // 直接顺序遍历一次更划算。
        if (want * 4 >= total) {
            for (const auto& [key, _] : expire_map_) {
                candidates.push_back(key);
                if (candidates.size() >= want) break;
            }
        } else {
            auto it = expire_map_.begin();
            size_t last = 0;
            for (size_t k = 0; k < want; ++k) {
                size_t target = dist(rng);
                if (target < last) {
                    // 单调化：避免来回扫描（重复取样不影响过期抽样的随机目的）
                    target = last;
                }
                std::advance(it, target - last);
                last = target;
                candidates.push_back(it->first);
            }
        }

        LOG_TRACE(EXPIRE, "Got %zu candidates (requested %d), total keys=%zu",
                 candidates.size(), n, total);

        return candidates;
    }

    size_t ExpireDict::delete_expired() {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        const size_t before_count = expire_map_.size();
        const int64_t now = current_time_ms();
        size_t deleted_count = 0;

        for (auto it = expire_map_.begin(); it != expire_map_.end();) {
            if (now >= it->second) { // 已过期
                LOG_DEBUG(EXPIRE, "Deleting expired key=%s, expired_at=%ld",
                         it->first.c_str(), it->second);
                it = expire_map_.erase(it);
                ++deleted_count;
            } else {
                ++it;
            }
        }

        if (deleted_count > 0) {
            LOG_INFO(EXPIRE, "Deleted %zu expired keys, remaining=%zu",
                    deleted_count, expire_map_.size());
        } else if (before_count > 0) {
            LOG_TRACE(EXPIRE, "No keys expired, checked %zu keys", before_count);
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
