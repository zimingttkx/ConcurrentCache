#include "config.h"
#include <algorithm>
#include <sstream>

namespace cc_server {

// 构造函数

Config::Config() : config_file_("./conf/concurrentcache.conf") {
    // 默认不加载，让调用者决定何时 load
}

// 单例

Config& Config::instance() {
    // C++11 static 局部变量初始化线程安全
    static Config instance;
    return instance;
}

// 配置加载

bool Config::loadInternal() {
    std::ifstream file(config_file_);
    if (!file.is_open()) {
        // 打不开就是打不开，交给调用方决定是致命错误还是退回默认值——
        // 旧代码在这里静默返回、load() 再恒返回 true，导致 main 里
        // “配置文件加载失败”的分支永远走不到，拼错路径等于没配。
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;

        // 解析 key = value
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // 去除首尾空白
            trim(key);
            trim(value);

            config_data_[key] = value;
        }
    }
    file.close();
    return true;
}

bool Config::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_file_ = filename;
    config_data_.clear();

    const bool loaded = loadInternal();

    // 设置日志默认值
    // 如果配置文件中没有指定，使用这些默认值
    if (config_data_.find("log_level") == config_data_.end()) {
        config_data_["log_level"] = "info";
    }
    if (config_data_.find("log_file") == config_data_.end()) {
        config_data_["log_file"] = "./logs/concurrentcache.log";
    }
    if (config_data_.find("log_max_size") == config_data_.end()) {
        config_data_["log_max_size"] = "104857600";  // 100MB
    }
    if (config_data_.find("log_max_files") == config_data_.end()) {
        config_data_["log_max_files"] = "5";
    }

    return loaded;
}

void Config::reload() {
    // 快照（配置 + 观察者列表）都在锁内完成，锁外只回调。
    // 修复 P2（数据竞争）：旧代码锁外遍历 observers_（config_data_ 已快照，
    // 但观察者 map 本身没有）——与 addObserver/removeObserver 并发时是 UB。
    std::vector<std::pair<std::string, std::string>> pending_notifications;
    std::map<std::string, std::vector<ConfigObserver*>> observers_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 重新加载配置
        config_data_.clear();
        loadInternal();

        // 设置日志默认值
        if (config_data_.find("log_level") == config_data_.end()) {
            config_data_["log_level"] = "info";
        }
        if (config_data_.find("log_file") == config_data_.end()) {
            config_data_["log_file"] = "./logs/concurrentcache.log";
        }
        if (config_data_.find("log_max_size") == config_data_.end()) {
            config_data_["log_max_size"] = "104857600";
        }
        if (config_data_.find("log_max_files") == config_data_.end()) {
            config_data_["log_max_files"] = "5";
        }

        // 收集通知数据与观察者快照
        for (auto& kv : config_data_) {
            pending_notifications.push_back({kv.first, kv.second});
        }
        observers_snapshot = observers_;
    }
    // 在锁外调用观察者，避免死锁；遍历的是快照，无并发问题
    for (auto& [key, val] : pending_notifications) {
        auto it = observers_snapshot.find(key);
        if (it != observers_snapshot.end()) {
            for (auto* obs : it->second) {
                obs->onConfigChange(key, val);
            }
        }
    }
}

// 配置获取

std::string Config::getString(const std::string& key, const std::string& default_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::getInt(const std::string& key, int default_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        try {
            return std::stoi(it->second);
        } catch (...) {
            // 转换失败，返回默认值
        }
    }
    return default_value;
}

bool Config::getBool(const std::string& key, bool default_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        return it->second == "true" || it->second == "1";
    }
    return default_value;
}

// 观察者模式

void Config::addObserver(const std::string& key, ConfigObserver* observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_[key].push_back(observer);
}

void Config::removeObserver(const std::string& key, ConfigObserver* observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = observers_.find(key);
    if (it == observers_.end()) return;

    auto& observerList = it->second;
    observerList.erase(
        std::remove(observerList.begin(), observerList.end(), observer),
        observerList.end()
    );
}

void Config::notifyObservers(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = observers_.find(key);
    if (it == observers_.end()) return;

    auto valueIt = config_data_.find(key);
    if (valueIt == config_data_.end()) return;

    for (auto& observer : it->second) {
        observer->onConfigChange(key, valueIt->second);
    }
}

// 集群配置

bool Config::clusterEnabled() const {
    return getBool("cluster_enabled", false);
}

std::string Config::clusterConfigFile() const {
    return getString("cluster_config_file", "nodes.conf");
}

int Config::clusterNodeTimeout() const {
    return getInt("cluster_node_timeout", 15000);
}

int Config::clusterReplicaValidityFactor() const {
    return getInt("cluster_replica_validity_factor", 10);
}

bool Config::clusterRequireFullCoverage() const {
    return getBool("cluster_require_full_coverage", false);
}

std::string Config::clusterBindAddr() const {
    return getString("cluster_bind_addr", "127.0.0.1");
}

// 工具函数

void Config::trim(std::string& s) {
    if (s.empty()) return;
    // 去除首部空白
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    // 去除尾部空白
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
}

} // namespace cc_server
