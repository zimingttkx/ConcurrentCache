#ifndef CONCURRENTCACHE_BASE_CONFIG_H
#define CONCURRENTCACHE_BASE_CONFIG_H

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include "log.h"

namespace cc_server {
    /**
     * @brief Config类：配置文件解析器
     *
     * 协作关系：
     * - 被服务器启动时加载，读取 port、thread_num 等配置
     * - 使用单例模式，全局访问配置
     * - 依赖 Logger 记录错误日志
     */
    class Config {
    public:
        // 获取单例实例
        static Config &getInstance();

        // 加载配置文件
        bool load(const std::string &filename);

        // 获取配置项
        std::string getString(const std::string &key, const std::string &default_value = "");

        int getInt(const std::string &key, int default_value = 0);

    private:
        Config();

        void trim(std::string &s);

        std::unordered_map<std::string, std::string> config_data_;
    };
}

#endif // CONCURRENTCACHE_BASE_CONFIG_H
