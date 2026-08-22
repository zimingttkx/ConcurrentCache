#ifndef CONCURRENTCACHE_CACHE_STORAGE_H
#define CONCURRENTCACHE_CACHE_STORAGE_H

#include <optional>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <atomic>
#include <chrono>
#include "expire_dict.h"
#include "datatype/object.h"

namespace cc_server {
    // 缓存条目结构体
    struct CacheEntry {
        CacheObject value;  // 替换原来的 std::string value
        std::atomic<int64_t> last_access_time_ms;  // 原子：读路径可能并发更新（修复 P1-4）
        int64_t expire_at_ms = -1;  // 绝对过期时间戳（毫秒），-1 表示永不过期（修复 P1-5 单锁一致性）

        CacheEntry() : last_access_time_ms(0) {}
        CacheEntry(const CacheObject& v, int64_t t) : value(v), last_access_time_ms(t) {}

        // std::atomic 不可拷贝/移动，需手动实现（否则 unordered_map 的
        // insert_or_assign/emplace 无法编译）
        CacheEntry(CacheEntry&& o) noexcept
            : value(std::move(o.value)),
              last_access_time_ms(o.last_access_time_ms.load(std::memory_order_relaxed)),
              expire_at_ms(o.expire_at_ms) {}
        CacheEntry& operator=(CacheEntry&& o) noexcept {
            if (this != &o) {
                value = std::move(o.value);
                last_access_time_ms.store(o.last_access_time_ms.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                expire_at_ms = o.expire_at_ms;
            }
            return *this;
        }
        CacheEntry(const CacheEntry& o)
            : value(o.value),
              last_access_time_ms(o.last_access_time_ms.load(std::memory_order_relaxed)),
              expire_at_ms(o.expire_at_ms) {}
        CacheEntry& operator=(const CacheEntry& o) {
            if (this != &o) {
                value = o.value;
                last_access_time_ms.store(o.last_access_time_ms.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                expire_at_ms = o.expire_at_ms;
            }
            return *this;
        }
    };

    // 淘汰策略配置
    struct EvictionConfig {
        static constexpr size_t kMaxEntries = 2000000; // 最大键数量
        static constexpr double kEvictThreshold = 0.9; // 淘汰触发阈值（占用率）
        static constexpr double kEvictTargetRatio = 0.6; // 淘汰目标占用率（留出 40% 空间）
    };

    // 用于 RDB 持久化的键值对结构体（包含过期时间）
    struct KVWithTTL {
        std::string key;
        CacheObject value;
        int64_t expire_time_ms;  // 过期时间戳（毫秒），-1 表示永不过期

        KVWithTTL(const std::string& k, const CacheObject& v, int64_t e)
            : key(k), value(v), expire_time_ms(e) {}
    };

    /**
     * @brief GlobalStorage 类 - 全局键值存储（单例）
     *
     * 设计目标：
     * - 线程安全：分段锁确保线程安全的同时 支持高并发
     * - 单例模式：全局只有一个存储实例
     * - O(1) 平均查找复杂度
     */
    class GlobalStorage {
    public:
        /** @brief 获取单例实例（线程安全） */
        static GlobalStorage& instance();

        /** @brief 获取键对应的值，不存在返回空字符串 */
        std::optional<CacheObject> get(const std::string& key);

        /** @brief 设置键值对，键已存在则更新 */
        void set(const std::string& key, const CacheObject& value);

        /** @brief 删除键值对，返回是否删除成功 */
        bool del(const std::string& key);

        /** @brief 检查键是否存在 */
        bool exist(const std::string& key) const;

        /** @brief 获取存储的键值对数量 */
        size_t size() const;

        /** @brief 清空所有键值对 */
        void clear();

        // 禁用拷贝
        GlobalStorage(const GlobalStorage&) = delete;
        GlobalStorage& operator=(const GlobalStorage&) = delete;

        // 获取过期字典的引用，供命令使用
        ExpireDict& expire_dict() {
            return expire_dict_;
        }
        // 检查是否过期
        bool is_expired(const std::string& key) const {
            return expire_dict_.is_expired(key);
        }

        // ARU 方法

        // 获取最大键数量
        size_t max_entries() const { return max_entries_; }

        // 设置最大键数量
        void set_max_entries(size_t max_entries) {
            max_entries_ = max_entries;
        }

        // 尝试淘汰一个最久没有访问过的key
        std::string evict_one();

        // 检查是否需要淘汰 必要时触发淘汰
        void evict_if_needed(const std::string& hint_key = "");

        // 获取所有对象 用于RDB持久化（不带 TTL）
        std::vector<std::pair<std::string, CacheObject>> get_all_objects() const;

        // 获取所有对象用于 RDB 持久化（带 TTL）
        std::vector<KVWithTTL> get_all_objects_with_ttl() const;

        /**
         * @brief 原子性设置键值对和过期时间
         * @param key 键
         * @param value 值
         * @param ttl_ms 过期时间（毫秒），<=0 表示永不过期
         * @note 用于 SETEX 等需要原子性设置值和过期时间的场景
         */
        void set_with_expire(const std::string& key, const CacheObject& value, int64_t ttl_ms);

        // 设置键的过期时间（供 RDB 加载时使用）
        void set_expire(const std::string& key, int64_t ttl_ms);

        // 获取当前时间戳（毫秒）- 公开给 RdbPersistence 使用
        int64_t current_time_ms() const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /**
         * @brief 原子增加/减少键值（用于 INCR/DECR/INCRBY/DECRBY）
         * @param key 键
         * @param delta 增量（可正可负）
         * @return 成功返回新值；键不存在则从 0 起算；非整数值/溢出返回 nullopt
         * @note 在 shard 独占锁内完成读-改-写，保证并发 INCR 不丢更新
         */
        std::optional<int64_t> incrby(const std::string& key, int64_t delta);

        // 脏计数器相关方法
        void increment_dirty() { dirty_counter_.fetch_add(1, std::memory_order_relaxed); }
        size_t get_dirty_count() const { return dirty_counter_.load(std::memory_order_relaxed); }
        void reset_dirty_count() { dirty_counter_.store(0, std::memory_order_relaxed); }
        void decrement_dirty_count(size_t count) {
            dirty_counter_.fetch_sub(count, std::memory_order_relaxed);
        }

        // 构造函数 - 允许创建实例用于测试
        GlobalStorage();

    private:

        // 根据key计算应该去哪个分片
        size_t get_shard_index(const std::string& key) const {
            return std::hash<std::string>{}(key) % num_shards_;
        }

        // 最大键数量
        size_t max_entries_{EvictionConfig::kMaxEntries};

        // 脏计数器（自上次 BGSAVE 以来的写操作次数）
        std::atomic<size_t> dirty_counter_{0};

        // 分段锁的默认分片数量 一般为CPU核心数量 * 2
        static constexpr size_t kDefaultShards = 64;

        // 分片数量
        size_t num_shards_{kDefaultShards};

        // 分片存储数组（存储 CacheEntry 而非 std::string）
        std::vector<std::unordered_map<std::string, CacheEntry>> stores_;

        // 分片锁数组 每个分片对应一把shared_mutex
        mutable std::unique_ptr<std::shared_mutex[]> mutexes_;

        ExpireDict expire_dict_;
    };
}

#endif
