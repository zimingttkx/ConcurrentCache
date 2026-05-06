#ifndef CONCURRENTCACHE_CACHE_EXPIRE_DICT_H
#define CONCURRENTCACHE_CACHE_EXPIRE_DICT_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <random>


namespace cc_server {

/**
 * @brief ExpireDict 类 - 过期字典
 *
 * 管理 key → 过期时间戳(ms) 的映射
 * 线程安全，支持多读单写
 */
class ExpireDict {
public:
    // 构造函数
    ExpireDict();

    // 禁用拷贝
    ExpireDict(const ExpireDict&) = delete;
    ExpireDict& operator=(const ExpireDict&) = delete;

    /**
     * @brief 设置键的过期时间（从当前时间起 + expire_ms 毫秒）
     * @param key 键
     * @param expire_ms 过期时长（毫秒）
     */
    void set(const std::string& key, int64_t expire_ms);

    /**
     * @brief 获取键的剩余生存时间
     * @param key 键
     * @return 剩余时间(ms) 或 负数表示特殊状态
     *         正数：剩余生存时间（毫秒）
     *         -2：键不存在 或 已过期（两者都返回 -2）
     */
    int64_t get_ttl(const std::string& key) const;

    /**
     * @brief 获取键的过期时间戳
     * @param key 键
     * @return 过期时间戳(ms) 或 -1 表示键不存在（无过期设置）
     */
    int64_t get_expire_time(const std::string& key) const;

    /**
     * @brief 移除键的过期时间（变成永不过期）
     * @param key 键
     * @return true=成功，false=键不存在
     */
    bool persist(const std::string& key);

    /**
     * @brief 删除键的过期记录
     * @param key 键
     */
    void remove(const std::string& key);

    /**
     * @brief 检查键是否已过期
     * @param key 键
     * @return true=已过期或不存在，false=未过期
     */
    bool is_expired(const std::string& key) const;

    /**
     * @brief 随机获取候选键（用于定期删除）
     * @param n 获取数量
     * @return 候选键列表
     */
    std::vector<std::string> get_candidates(int n) const;

    /**
     * @brief 删除所有已过期的键
     * @return 删除的数量
     */
    size_t delete_expired();

    /**
     * @brief 获取当前记录的键数量
     */
    size_t size() const;

    /**
     * @brief 检查键是否存在（不考虑是否过期）
     */
    bool contains(const std::string& key) const;

    /**
     * @brief 清空所有过期记录
     */
    void clear_all();

private:
    // 获取当前时间戳（毫秒）
    static int64_t current_time_ms();

    std::unordered_map<std::string, int64_t> expire_map_;  // key → 过期时间戳(ms)
    mutable std::shared_mutex mutex_;                        // 线程安全
};

}  // namespace cc_server

#endif
