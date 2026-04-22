#ifndef CONCURRENTCACHE_CACHE_STORAGE_H
#define CONCURRENTCACHE_CACHE_STORAGE_H

#include <string>
#include <unordered_map>
#include <shared_mutex>

namespace cc_server {

    /**
     * @brief GlobalStorage 类 - 全局键值存储（单例）
     *
     * 设计目标：
     * - 线程安全：读写锁保护并发访问
     * - 单例模式：全局只有一个存储实例
     * - O(1) 平均查找复杂度
     */
    class GlobalStorage {
    public:
        /** @brief 获取单例实例（线程安全） */
        static GlobalStorage& instance();

        /** @brief 获取键对应的值，不存在返回空字符串 */
        std::string get(const std::string& key);

        /** @brief 设置键值对，键已存在则更新 */
        void set(const std::string& key, const std::string& value);

        /** @brief 删除键值对，返回是否删除成功 */
        bool del(const std::string& key);

        /** @brief 检查键是否存在 */
        bool exist(const std::string& key);

        /** @brief 获取存储的键值对数量 */
        size_t size() const;

        /** @brief 清空所有键值对 */
        void clear();

        // 禁用拷贝
        GlobalStorage(const GlobalStorage&) = delete;
        GlobalStorage& operator=(const GlobalStorage&) = delete;

    private:
        GlobalStorage() = default;

        std::unordered_map<std::string, std::string> store_;  // 底层哈希表存储
        mutable std::shared_mutex mutex_;                     // 读写锁
    };
}

#endif
