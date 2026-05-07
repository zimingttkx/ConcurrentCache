#ifndef CONCURRENTCACHE_CACHE_STORAGE_H
#define CONCURRENTCACHE_CACHE_STORAGE_H

#include <optional>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include "expire_dict.h"
#include "datatype/object.h"

namespace cc_server {
    // 缓存条目结构体
    struct CacheEntry {
        CacheObject value;  // 替换原来的 std::string value
        int64_t last_access_time_ms;
        CacheEntry() : last_access_time_ms(0) {}
        CacheEntry(const CacheObject& v, int64_t t) : value(v), last_access_time_ms(t) {}
    };

    // 淘汰策略配置
    struct EvictionConfig {
        static constexpr size_t kMaxEntries = 100000; // 最大键数量
        static constexpr double kEvictThreshold = 0.9; // 淘汰触发阈值（占用率）
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
    private:

        // 获取当前时间戳
        int64_t current_time_ms() const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        GlobalStorage();

        // 根据key计算应该去哪个分片
        size_t get_shard_index(const std::string& key) const {
            return std::hash<std::string>{}(key) % num_shards_;
        }

        // 最大键数量
        size_t max_entries_{EvictionConfig::kMaxEntries};


        // 分段锁的默认分片数量 一般为CPU核心数量 * 2
        static constexpr size_t kDefaultShards = 64;

        // 分片数量
        size_t num_shards_{kDefaultShards};

        // 分片存储数组（存储 CacheEntry 而非 std::string）
        std::vector<std::unordered_map<std::string, CacheEntry>> stores_;

        // 分片锁数组 每个分片对应一把shared_mutex
        mutable std::vector<std::shared_mutex> mutexes_;

        ExpireDict expire_dict_;
    };
}

#endif
