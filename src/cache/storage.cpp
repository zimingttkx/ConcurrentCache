#include "storage.h"
#include "base/log.h"
#include <mutex>
#include <cassert>

namespace cc_server {

    GlobalStorage::GlobalStorage() {
        stores_.reserve(kDefaultShards);
        mutexes_.reserve(kDefaultShards);

        for (size_t i = 0; i < kDefaultShards; i++) {
            stores_.emplace_back();  // 初始化每个分片的哈希表
            mutexes_.emplace_back();
        }

        // 防御性检查：确保初始化成功
        assert(stores_.size() == kDefaultShards && "GlobalStorage - stores_ init failed");
        assert(mutexes_.size() == kDefaultShards && "GlobalStorage - mutexes_ init failed");

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

    std::optional<std::string> GlobalStorage::get(const std::string& key) {
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

        // 惰性删除：检查是否过期
        if (expire_dict_.is_expired(key)) {
            LOG_DEBUG(STORAGE, "Get key=%s - expired, triggering lazy delete", key.c_str());
            lock.unlock();
            del(key);
            return std::nullopt;
        }

        // 更新访问时间
        it->second.last_access_time_ms = current_time_ms();

        LOG_TRACE(STORAGE, "Get key=%s - found, shard=%zu", key.c_str(), shard_idx);
        return it->second.value;
    }

    // set - 设置键值对
    // @param key 键
    // @param value 值
    // @note 线程安全：使用独占锁，写操作互斥
    // @note 键已存在则更新值，不存在则插入新键值对

    void GlobalStorage::set(const std::string& key, const std::string& value) {
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

        // 在对应分片的unordered_map里面插入或者更新
        stores_[shard_idx].insert_or_assign(key, CacheEntry(value, now));

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
        return stores_[shard_idx].contains(key);
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
            stores_[oldest_shard].erase(oldest_key);
        }

        // 同时删除过期记录
        expire_dict_.remove(oldest_key);

        LOG_INFO(CACHE, "Evicted key=%s, shard=%zu, last_access=%ldms ago",
                oldest_key.c_str(), oldest_shard, now - oldest_time);

        return oldest_key;
    }

    void GlobalStorage::evict_if_needed(const std::string& hint_key) {
        // 防御性检查：阈值必须有效
        size_t threshold = static_cast<size_t>(max_entries_ * EvictionConfig::kEvictThreshold);
        assert(threshold <= max_entries_ && "GlobalStorage::evict_if_needed - threshold exceeds max");

        size_t current_size = size();

        // 未超过阈值，不需要淘汰
        if (current_size < threshold) {
            return;
        }

        LOG_WARN(CACHE, "Cache full (size=%zu, max=%zu, threshold=%zu), starting eviction",
                current_size, max_entries_, threshold);

        // 淘汰到安全线（留出 20% 空间）
        size_t target_size = static_cast<size_t>(max_entries_ * 0.6);

        // 防御性检查：目标大小必须有效
        assert(target_size < max_entries_ && "GlobalStorage::evict_if_needed - target_size invalid");

        size_t evicted_count = 0;
        while (size() > target_size) {
            std::string evicted = evict_one();
            if (evicted.empty()) {
                LOG_WARN(CACHE, "Eviction stopped - no more keys to evict, evicted=%zu", evicted_count);
                break;
            }
            ++evicted_count;
            LOG_DEBUG(CACHE, "Evicted key during auto-eviction: %s", evicted.c_str());
        }

        LOG_INFO(CACHE, "Eviction complete - evicted %zu keys, current_size=%zu, target_size=%zu",
                evicted_count, size(), target_size);
    }
}
