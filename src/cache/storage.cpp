#include "storage.h"
#include <mutex>

namespace cc_server {

    GlobalStorage::GlobalStorage() {
        stores_.reserve(kDefaultShards);
        mutexes_.reserve(kDefaultShards);

        for (size_t i = 0; i < kDefaultShards; i++) {
            stores_.emplace_back();  // 初始化每个分片的哈希表
            mutexes_.emplace_back();
        }
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
        // 步骤1: 计算 key 应该去哪个分片
        const size_t shard_idx = get_shard_index(key);

        // 步骤2: 在对应分片加共享锁（多个读可以同时进行）
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 步骤3: 在对应分片的 unordered_map 中查找
        auto& store = stores_[shard_idx];

        // 找到返回对应值，找不到返回 nullopt
        if (const auto it = store.find(key); it != store.end()) {
            return it->second;
        }
        return std::nullopt;  // 使用 std::nullopt 表示不存在
    }

    
    // set - 设置键值对
    // @param key 键
    // @param value 值
    // @note 线程安全：使用独占锁，写操作互斥
    // @note 键已存在则更新值，不存在则插入新键值对
    
    void GlobalStorage::set(const std::string& key, const std::string& value) {
        // 1. 计算应该去哪个分片
        const size_t shard_idx  = get_shard_index(key);
        // 2. 在对应的分片加独占锁
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);  // 独占锁：写时阻塞所有读写
        // 在对应分片的unordered_map里面插入或者更新
        stores_[shard_idx].emplace(key, value);
    }

    
    // del - 删除键值对
    // @param key 键
    // @return true 表示键存在且被删除，false 表示键不存在
    // @note 线程安全：使用独占锁
    
    bool GlobalStorage::del(const std::string& key) {
        const size_t shard_idx  = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);
        return stores_[shard_idx].erase(key) > 0;
    }

    
    // exist - 检查键是否存在
    // @param key 键
    // @return true 键存在，false 键不存在
    // @note 线程安全：使用独占锁
    
    bool GlobalStorage::exist(const std::string& key) const {
        const size_t shard_idx  = get_shard_index(key);
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);
        return stores_[shard_idx].contains(key) > 0;
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

        return total;
    }

    
    // clear - 清空所有键值对
    // @note 线程安全：遍历所有分片，每个分片加独占锁
    // @note 时间复杂度 O(num_shards + total_elements)
    //
    // 注意：这是一个代价较高的操作，需要锁定所有分片

    void GlobalStorage::clear() {
        // 遍历所有分片
        for (size_t i = 0; i < num_shards_; ++i) {
            // 对每个分片加独占锁
            std::unique_lock<std::shared_mutex> lock(mutexes_[i]);
            stores_[i].clear();
        }
    }

}
