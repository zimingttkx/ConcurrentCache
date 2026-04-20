#include "config.h"
#include <algorithm>

namespace cc_server {
    // 默认构造函数
    Config::Config() = default;

    // 获取单例实例
    Config &Config::getInstance() {
        static Config instance;
        return instance;
    }

    // 读取配置文件
    // - 打开文件，解析 key = value 格式
    // - 跳过注释(#开头)和空行
    // - 调用 trim() 去除键值首尾空格后存入 map
    bool Config::load(const std::string &filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file:" + filename);
            return false;
        }
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                trim(key);
                trim(value);
                config_data_[key] = value;
            }
        }
        file.close();
        LOG_INFO("Config file loaded successfully:" + filename);
        return true;
    }

    // 获取字符串配置项，不存在则返回默认值
    std::string Config::getString(const std::string &key, const std::string &default_value) {
        if (config_data_.find(key) != config_data_.end()) {
            return config_data_[key];
        }
        return default_value;
    }

    // 获取整数配置项，转换失败记录错误并返回默认值
    int Config::getInt(const std::string &key, int default_value) {
        if (config_data_.find(key) != config_data_.end()) {
            try {
                return std::stoi(config_data_[key]);
            } catch (...) {
                LOG_ERROR("Invalid integer value for key:" + key);
            }
        }
        return default_value;
    }

    // 去除字符串首尾的空白字符
    void Config::trim(std::string &s) {
        if (s.empty()) return;
        s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
        s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
    }
}
