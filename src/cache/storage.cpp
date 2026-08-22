#include "storage.h"
#include "base/log.h"
#include <mutex>
#include <cassert>
#include <limits>

namespace cc_server {

    GlobalStorage::GlobalStorage() {
        stores_.resize(kDefaultShards);
        mutexes_ = std::make_unique<std::shared_mutex[]>(kDefaultShards);

        // 防御性检查：确保初始化成功
        assert(stores_.size() == kDefaultShards && "GlobalStorage - stores_ init failed");

        LOG_INFO(STORAGE, "GlobalStorage initialized with %zu shards", kDefaultShards);
    }

    // 单例模式：Magic Static（C++11 线程安全局部静态变量）
    // 多个线程同时调用只会初始化一次
    GlobalStorage& GlobalStorage::instance() {
        static GlobalStorage instance;
        return instance;
    }

    // get - 获取键对应的值
    // @param key 键
    // @return 键对应的值，不存在返回空字符串
    // @note 线程安全：使用共享锁，允许多读并发

    std::optional<CacheObject> GlobalStorage::get(const std::string& key) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::get - key is empty");

        // 步骤1: 计算 key 应该去哪个分片
        const size_t shard_idx = get_shard_index(key);

        // 步骤2: 在对应分片加共享锁（多个读可以同时进行）
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 步骤3: 在对应分片的 unordered_map 中查找
        auto& store = stores_[shard_idx];

        auto it = store.find(key);
        if (it == store.end()) {
            LOG_DEBUG(STORAGE, "Get key=%s - not found", key.c_str());
            return std::nullopt;
        }

        // 惰性删除：检查是否过期（以 CacheEntry.expire_at_ms 为准，避免两锁竞态）
        if (expire_dict_.is_expired(key) ||
            (it->second.expire_at_ms > 0 && current_time_ms() >= it->second.expire_at_ms)) {
            LOG_DEBUG(STORAGE, "Get key=%s - expired, triggering lazy delete", key.c_str());
            // 注：expire_at_ms 改为 atomic 后，这里可去掉 expire_dict_ 的二次查询，
            // 但为了兼容现有主动过期路径先保留双判。
            lock.unlock();
            del(key);
            return std::nullopt;
        }

        // 更新访问时间（原子写，避免共享锁下的数据竞争；修复 P1-4）
        // 读取 LRU 时间戳仅在 evict_one 的 pass2 独占锁下发生，此处写与读无并发竞争。
        it->second.last_access_time_ms.store(current_time_ms(), std::memory_order_relaxed);

        LOG_TRACE(STORAGE, "Get key=%s - found, shard=%zu", key.c_str(), shard_idx);
        return it->second.value;
    }

    // set - 设置键值对
    // @param key 键
    // @param value 值
    // @note 线程安全：使用独占锁，写操作互斥
    // @note 键已存在则更新值，不存在则插入新键值对

    void GlobalStorage::set(const std::string& key, const CacheObject& value) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::set - key is empty");

        // 检查是否需要淘汰（在获取锁之前检查，避免长时间持锁）
        evict_if_needed(key);

        // 1. 计算应该去哪个分片
        const size_t shard_idx  = get_shard_index(key);
        // 2. 在对应的分片加独占锁
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);  // 独占锁：写时阻塞所有读写

        // 如果键已经存在 清除过期时间
        bool had_expiration = expire_dict_.contains(key);
        expire_dict_.remove(key);

        // 获取当前时间
        int64_t now = current_time_ms();

        // 在对应分片的unordered_map里面插入或者更新（同时维护 expire_at_ms，保持单真相源）
        CacheEntry entry(value, now);
        entry.expire_at_ms = -1;  // set 命令清除过期
        stores_[shard_idx].insert_or_assign(key, std::move(entry));

        // 增加脏计数器
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);

        if (had_expiration) {
            LOG_DEBUG(STORAGE, "Set key=%s - value updated, cleared expiration, shard=%zu",
                     key.c_str(), shard_idx);
        } else {
            LOG_DEBUG(STORAGE, "Set key=%s - new key inserted, shard=%zu",
                     key.c_str(), shard_idx);
        }
    }

    // del - 删除键值对
    // @param key 键
    // @return true 表示键存在且被删除，false 表示键不存在
    // @note 线程安全：使用独占锁

    bool GlobalStorage::del(const std::string& key) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::del - key is empty");

        const size_t shard_idx  = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 删除过期记录
        expire_dict_.remove(key);

        // 删除键值对
        size_t erased = stores_[shard_idx].erase(key);

        if (erased > 0) {
            // 增加脏计数器
            dirty_counter_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG(STORAGE, "Delete key=%s - success, shard=%zu", key.c_str(), shard_idx);
        }

        return erased > 0;
    }


    // exist - 检查键是否存在
    // @param key 键
    // @return true 键存在，false 键不存在
    // @note 线程安全：使用共享锁

    bool GlobalStorage::exist(const std::string& key) const {
        assert(!key.empty() && "GlobalStorage::exist - key is empty");

        const size_t shard_idx  = get_shard_index(key);
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);
        bool in_store = stores_[shard_idx].contains(key);
        if (!in_store) return false;
        // 一致性修复（P1-12）：已过期未删除的键不应被 EXIST/DBSIZE 计入
        if (expire_dict_.is_expired(key)) return false;
        return true;
    }


    // size - 获取存储的键值对数量
    // @return 当前存储的元素数量
    // @note 线程安全：使用共享锁，允许多读并发

    size_t GlobalStorage::size() const {
        size_t total = 0;
        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            total += stores_[i].size();
        }

        LOG_TRACE(STORAGE, "Size query - total=%zu", total);
        return total;
    }


    // clear - 清空所有键值对
    // @note 线程安全：遍历所有分片，每个分片加独占锁
    // @note 时间复杂度 O(num_shards + total_elements)
    //
    // 注意：这是一个代价较高的操作，需要锁定所有分片

    void GlobalStorage::clear() {
        size_t total_cleared = 0;

        // 遍历所有分片
        for (size_t i = 0; i < num_shards_; ++i) {
            // 对每个分片加独占锁
            std::unique_lock<std::shared_mutex> lock(mutexes_[i]);
            total_cleared += stores_[i].size();
            stores_[i].clear();
        }
        // 同时清除所有过期记录
        expire_dict_.clear_all();

        LOG_INFO(STORAGE, "Clear all - cleared %zu keys", total_cleared);
    }

    /*
     *
     * ARU 淘汰方法
     *
     */
    std::string GlobalStorage::evict_one() {
        const int64_t now = current_time_ms();
        std::string oldest_key;
        int64_t oldest_time = now;
        size_t oldest_shard = 0;
        bool found = false;

        // 第一遍：找到最老的键
        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            for (const auto& [key, entry] : stores_[i]) {
                if (!found || entry.last_access_time_ms < oldest_time) {
                    oldest_time = entry.last_access_time_ms;
                    oldest_key = key;
                    oldest_shard = i;
                    found = true;
                }
            }
        }

        // 没有键可以淘汰
        if (!found || oldest_key.empty()) {
            LOG_WARN(CACHE, "Evict_one - no key found to evict, found=%d, key_empty=%d",
                    found, oldest_key.empty());
            return "";
        }

        // 防御性检查：确保找到的键有效
        assert(oldest_shard < num_shards_ && "GlobalStorage::evict_one - shard index out of bounds");
        assert(!oldest_key.empty() && "GlobalStorage::evict_one - oldest_key is empty");

        // 第二遍：从最老的分片删除该键
        {
            std::unique_lock<std::shared_mutex> lock(mutexes_[oldest_shard]);
            // TOCTOU 修复（P1-6）：重新校验该键仍存在且 last_access_time_ms 未被并发更新。
            // 若键已消失或访问时间已改变，则本次不淘汰（避免假淘汰或误删热键）。
            auto it = stores_[oldest_shard].find(oldest_key);
            if (it != stores_[oldest_shard].end() &&
                it->second.last_access_time_ms.load(std::memory_order_relaxed) == oldest_time) {
                stores_[oldest_shard].erase(it);
            } else {
                // 键状态已变化，本次跳过重试，交由下一轮 evict_if_needed 处理
                return "";
            }
        }

        // 同时删除过期记录
        expire_dict_.remove(oldest_key);

        LOG_INFO(CACHE, "Evicted key=%s, shard=%zu, last_access=%ldms ago",
                oldest_key.c_str(), oldest_shard, now - oldest_time);

        return oldest_key;
    }

    void GlobalStorage::evict_if_needed(const std::string& hint_key) {
        (void)hint_key; // unused
        // 防御性检查：阈值必须有效
        size_t threshold = static_cast<size_t>(static_cast<double>(max_entries_) * EvictionConfig::kEvictThreshold);
        assert(threshold <= max_entries_ && "GlobalStorage::evict_if_needed - threshold exceeds max");

        size_t current_size = size();

        // 未超过阈值，不需要淘汰
        if (current_size < threshold) {
            return;
        }

        LOG_WARN(CACHE, "Cache full (size=%zu, max=%zu, threshold=%zu), starting eviction",
                current_size, max_entries_, threshold);

        // 淘汰到安全线（留出 40% 空间）
        size_t target_size = static_cast<size_t>(static_cast<double>(max_entries_) * EvictionConfig::kEvictTargetRatio);

        // 防御性检查：目标大小必须有效
        assert(target_size < max_entries_ && "GlobalStorage::evict_if_needed - target_size invalid");

        size_t evicted_count = 0;
        while (current_size > target_size) {
            std::string evicted = evict_one();
            if (evicted.empty()) {
                LOG_WARN(CACHE, "Eviction stopped - no more keys to evict, evicted=%zu", evicted_count);
                break;
            }
            ++evicted_count;
            --current_size;
            LOG_DEBUG(CACHE, "Evicted key during auto-eviction: %s", evicted.c_str());
        }

        LOG_INFO(CACHE, "Eviction complete - evicted %zu keys, current_size=%zu, target_size=%zu",
                evicted_count, size(), target_size);
    }

    std::vector<std::pair<std::string, CacheObject> > GlobalStorage::get_all_objects() const {
        std::vector<std::pair<std::string, CacheObject>> result;
        result.reserve(size());

        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            for (const auto& [key, entry] : stores_[i]) {
                if (!expire_dict_.is_expired(key)) {
                    result.emplace_back(key, entry.value);
                }
            }
        }
        return result;
    }

    std::vector<KVWithTTL> GlobalStorage::get_all_objects_with_ttl() const {
        std::vector<KVWithTTL> result;
        result.reserve(size());

        // 获取当前时间
        int64_t now = current_time_ms();

        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            for (const auto& [key, entry] : stores_[i]) {
                // 获取过期时间
                int64_t expire_time_ms = expire_dict_.get_expire_time(key);

                // get_expire_time 返回 -1 表示永不过期
                // 如果有过期时间且已过期，则跳过
                if (expire_time_ms > 0 && now >= expire_time_ms) {
                    continue;  // 已过期，跳过
                }

                result.emplace_back(key, entry.value, expire_time_ms);
            }
        }
        return result;
    }

    void GlobalStorage::set_expire(const std::string& key, int64_t ttl_ms) {
        if (key.empty() || ttl_ms <= 0) return;

        // 设置过期时间
        expire_dict_.set(key, ttl_ms);
        LOG_DEBUG(STORAGE, "Set expire for key=%s, ttl_ms=%ld", key.c_str(), ttl_ms);
    }

    void GlobalStorage::set_with_expire(const std::string& key, const CacheObject& value, int64_t ttl_ms) {
        assert(!key.empty() && "GlobalStorage::set_with_expire - key is empty");

        // 检查是否需要淘汰
        evict_if_needed(key);

        // 计算应该去哪个分片
        const size_t shard_idx = get_shard_index(key);
        // 在对应的分片加独占锁
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 获取当前时间
        int64_t now = current_time_ms();

        // 设置值（同时维护 expire_at_ms，保持单真相源）
        CacheEntry entry(value, now);
        if (ttl_ms > 0) {
            entry.expire_at_ms = now + ttl_ms;
            expire_dict_.set_expire_time(key, entry.expire_at_ms);
            LOG_DEBUG(STORAGE, "Set_with_expire key=%s, ttl_ms=%ld, expire_at=%ld, shard=%zu",
                     key.c_str(), ttl_ms, entry.expire_at_ms, shard_idx);
        } else {
            entry.expire_at_ms = -1;
            expire_dict_.remove(key);
            LOG_DEBUG(STORAGE, "Set_with_expire key=%s (no expire), shard=%zu",
                     key.c_str(), shard_idx);
        }
        stores_[shard_idx].insert_or_assign(key, std::move(entry));

        // 增加脏计数器
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief 原子增加/减少键值（INCR/DECR/INCRBY/DECRBY 内部实现）
     *
     * 在 shard 独占锁内完成 读-改-写，保证并发 INCR 不丢更新。
     * 返回新值；若当前值非整数或递增会溢出 int64，返回 nullopt。
     */
    std::optional<int64_t> GlobalStorage::incrby(const std::string& key, int64_t delta) {
        assert(!key.empty() && "GlobalStorage::incrby - key is empty");

        // 淘汰检查（与 set 一致，避免长时间持锁前先做）
        evict_if_needed(key);

        const size_t shard_idx = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 惰性删除：若已过期，当作不存在（从 0 起算）
        if (expire_dict_.is_expired(key)) {
            stores_[shard_idx].erase(key);
            expire_dict_.remove(key);
        }

        int64_t val = 0;
        auto it = stores_[shard_idx].find(key);
        if (it != stores_[shard_idx].end()) {
            // 值必须是字符串类型且可解析为整数
            auto str_val = it->second.value.get_string();
            if (!str_val.has_value()) {
                return std::nullopt;  // 非字符串值（列表/哈希等）→ 调用方应回 WRONGTYPE
            }
            try {
                val = std::stoll(str_val.value());
            } catch (...) {
                return std::nullopt;  // 非整数 → 调用方应回 "value is not an integer"
            }
        }

        // 溢出检查：delta > 0 时 val > INT64_MAX - delta 会溢出
        if ((delta > 0 && val > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && val < std::numeric_limits<int64_t>::min() - delta)) {
            return std::nullopt;  // 溢出 → 调用方应回 "increment or decrement would overflow"
        }

        val += delta;

        int64_t now = current_time_ms();
        stores_[shard_idx].insert_or_assign(key, CacheEntry(CacheObject(std::to_string(val)), now));
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);

        return val;
    }

}
