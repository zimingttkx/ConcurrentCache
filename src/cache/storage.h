#ifndef CONCURRENTCACHE_CACHE_STORAGE_H
#define CONCURRENTCACHE_CACHE_STORAGE_H

#include <optional>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

namespace cc_server {

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
        std::optional<std::string> get(const std::string& key);

        /** @brief 设置键值对，键已存在则更新 */
        void set(const std::string& key, const std::string& value);

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

    private:
        GlobalStorage();

        // 根据key计算应该去哪个分片
        size_t get_shard_index(const std::string& key) const {
            return std::hash<std::string>{}(key) % num_shards_;
        }

        // 分段锁的默认分片数量 一般为CPU核心数量 * 2

        static constexpr size_t kDefaultShards = 64;

        // 分片数量
        size_t num_shards_{kDefaultShards};

        // 分片存储数组
        std::vector<std::unordered_map<std::string, std::string>>  stores_;

        // 分片锁数组 每个分片对应一把shared_mutex
        mutable std::vector<std::shared_mutex> mutexes_;
    };
}

#endif
