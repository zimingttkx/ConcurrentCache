#include "storage.h"
#include <mutex>

namespace cc_server {

    
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
        std::shared_lock<std::shared_mutex> lock(mutex_);  // 共享锁：多个读可并发
        auto it = store_.find(key);
        if (it != store_.end()) {
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
        std::unique_lock<std::shared_mutex> lock(mutex_);  // 独占锁：写时阻塞所有读写
        store_.emplace(key, value);
    }

    
    // del - 删除键值对
    // @param key 键
    // @return true 表示键存在且被删除，false 表示键不存在
    // @note 线程安全：使用独占锁
    
    bool GlobalStorage::del(const std::string& key) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return store_.erase(key) > 0;
    }

    
    // exist - 检查键是否存在
    // @param key 键
    // @return true 键存在，false 键不存在
    // @note 线程安全：使用独占锁
    
    bool GlobalStorage::exist(const std::string& key) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return store_.count(key) > 0;
    }

    
    // size - 获取存储的键值对数量
    // @return 当前存储的元素数量
    // @note 线程安全：使用共享锁，允许多读并发

    size_t GlobalStorage::size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);  // 共享锁：多个读可并发
        return store_.size();
    }

    
    // clear - 清空所有键值对
    // @note 线程安全：使用独占锁
    // @note 时间复杂度 O(n)，n 为当前元素数量
    
    void GlobalStorage::clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        store_.clear();
    }

}
