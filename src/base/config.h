#ifndef CONCURRENTCACHE_BASE_CONFIG_H
#define CONCURRENTCACHE_BASE_CONFIG_H
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include "log.h"

namespace cc_server {
    // 配置类 读取配置文件
    // 集中编码 避免硬编码 集中管理服务器运行参数 包括端口 日志级别
    class Config {
    public:
        // 获取单例的静态方法 确保全局只有一个Config对象 所以模块通过这个接口访问配置
        static Config& getInstance() {
            static Config instance;
            return instance;
        }
        // 读取配置文件
        // filename - 配置文件的路径
        // 返回值 成功返回true 失败返回false
        bool load(const std::string& filename) {
            // 打开文件
            std::ifstream file(filename);
            if (!file.is_open()) {
                // 如果打开失败就记录日志
                LOG_ERROR("Failed to open config file:" + filename);
                return false;
            }
            std::string line;
            while (std::getline(file, line)) {
                // 跳过注释和空行
                if (line.empty() || line[0] == '#') continue;
                // 解析Key = Value 格式
                size_t pos = line.find('=');
                if (pos != std::string::npos) {
                    // 提取等号左边的键
                    std::string key = line.substr(0, pos);
                    std::string value = line.substr(pos + 1);

                    // 去除键值首尾的空格
                    trim(key);
                    trim(value);
                    // 将键值对存入map
                    config_data_[key] = value;
                }
            }
            file.close();
            LOG_INFO("Config file loaded successfully:" + filename);
            return true;
        }

        // 获取字符串类型的配置项
        // 参数：key - 配置项的名称
        // default_value - 如果配置项不存在，返回的默认值
        // 返回值：配置项的值，或默认值
        std::string getString(const std::string& key, const std::string& default_value = " ") {
            // 如果配置项存在，返回配置项的值
            if (config_data_.find(key) != config_data_.end()) {
                return config_data_[key];
            }
            retrun default_value;
        }

        // 获取整数类型的配置项
        // 参数：key - 配置项的名称
        //       default_value - 如果配置项不存在或转换失败，返回的默认值
        // 返回值：配置项的整数值，或默认值
        int getInt(const std::string& key, int default_value = 0) {
            if (config_data_.find(key) != config_data_.end()) {
                try {
                    return std::stoi(config_data_[key]);
                }catch (...) {
                    // 如果转化失败说明存储的不是数字
                    LOG_ERROR("Invalid integer value for key:" + key);

                }
            }
            return default_value;
        }
    private:
        // 私有构造函数 : 防止外部构造函数
        Config() = default;

        // 辅助函数 : 去除字符串首尾的空白字符
        void trim(std::string & s) {
            if (s.empty()) return;
            // 去除开头的空白字符
            s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
            // 去除结尾的空白字符
            s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
        }

        // 存储配置项的map
        std::unordered_map<std::string, std::string> config_data_;
    };
}

#endif // CONCURRENTCACHE_BASE_CONFIG_H