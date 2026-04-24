#include "config.h"
#include <algorithm>
#include <sstream>

namespace cc_server {

// ==================== 构造函数 ====================

Config::Config() : config_file_("./conf/concurrentcache.conf") {
    // 默认不加载，让调用者决定何时 load
}

// ==================== 单例 ====================

Config& Config::instance() {
    // C++11 static 局部变量初始化线程安全
    static Config instance;
    return instance;
}

// ==================== 配置加载 ====================

void Config::loadInternal() {
    std::ifstream file(config_file_);
    if (!file.is_open()) {
        // 文件不存在，配置为空字典，稍后会用默认值
        return;
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
}

bool Config::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_file_ = filename;
    config_data_.clear();

    loadInternal();

    // ========== 设置日志默认值 ==========
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

    return true;
}

void Config::reload() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 重新加载配置
    config_data_.clear();
    loadInternal();

    // 通知所有观察者
    for (auto& kv : observers_) {
        const std::string& key = kv.first;
        auto it = config_data_.find(key);
        if (it != config_data_.end()) {
            for (auto& observer : kv.second) {
                observer->onConfigChange(key, it->second);
            }
        }
    }
}

// ==================== 配置获取 ====================

std::string Config::getString(const std::string& key, const std::string& default_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::getInt(const std::string& key, int default_value) {
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

bool Config::getBool(const std::string& key, bool default_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        return it->second == "true" || it->second == "1";
    }
    return default_value;
}

// ==================== 观察者模式 ====================

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

// ==================== 工具函数 ====================

void Config::trim(std::string& s) {
    if (s.empty()) return;
    // 去除首部空白
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    // 去除尾部空白
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
}

} // namespace cc_server
