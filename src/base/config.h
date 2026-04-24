#ifndef CONCURRENTCACHE_BASE_CONFIG_H
#define CONCURRENTCACHE_BASE_CONFIG_H

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace cc_server {

// ==================== ConfigObserver 接口 ====================

/**
 * @brief 配置观察者接口
 *
 * 为什么需要观察者模式？
 * - 当配置值变化时，需要通知相关的模块
 * - 比如 log_level 变了，Logger 要知道并更新
 * - 但 Config 不应该直接依赖 Logger（会循环依赖）
 *
 * 使用方式：
 * - 想要监听配置变化的类，实现这个接口
 * - 调用 Config::addObserver(key, this) 注册
 * - 配置变化时 Config 会调用 onConfigChange()
 */
class ConfigObserver {
public:
    virtual ~ConfigObserver() = default;

    /**
     * @brief 配置变更回调
     * @param key 配置项名称
     * @param value 配置项新值
     */
    virtual void onConfigChange(const std::string& key, const std::string& value) = 0;
};

// ==================== Config 类 ====================

/**
 * @brief Config类：配置文件解析器
 *
 * 版本2增强：
 * - 单例模式（线程安全）
 * - 观察者模式（支持热加载）
 * - 线程安全（所有操作加锁）
 * - 日志默认值设置
 *
 * 协作关系：
 * - 被服务器启动时加载，读取 port、thread_num 等配置
 * - 使用单例模式，全局访问配置
 * - 配置变化时通知观察者（如 Logger）
 */
class Config {
public:
    // 获取单例实例（C++11 static 线程安全）
    static Config& instance();

    // 加载配置文件
    // @param filename 配置文件路径
    // @return 加载是否成功
    bool load(const std::string& filename);

    // 重新加载配置（热加载）
    // 重新读取配置文件，并通知所有观察者
    void reload();

    // 获取配置项
    std::string getString(const std::string& key, const std::string& default_value = "");
    int getInt(const std::string& key, int default_value = 0);
    bool getBool(const std::string& key, bool default_value = false);

    // 观察者模式 - 注册观察者
    // @param key 要监听的配置项名
    // @param observer 观察者指针
    void addObserver(const std::string& key, ConfigObserver* observer);

    // 观察者模式 - 移除观察者
    void removeObserver(const std::string& key, ConfigObserver* observer);

    // 通知观察者（配置变化时调用）
    void notifyObservers(const std::string& key);

    // 禁用拷贝
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

private:
    // 私有构造函数
    Config();

    // 内部加载配置（被 load 和 reload 调用）
    void loadInternal();

    // 去除字符串首尾的空白字符
    void trim(std::string& s);

    // 配置数据
    std::unordered_map<std::string, std::string> config_data_;

    // 观察者列表：key -> 观察者列表
    std::map<std::string, std::vector<ConfigObserver*>> observers_;

    // 互斥锁（保护 config_data_ 和 observers_）
    std::mutex mutex_;

    // 配置文件路径
    std::string config_file_;
};

} // namespace cc_server

#endif // CONCURRENTCACHE_BASE_CONFIG_H
