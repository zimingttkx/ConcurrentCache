#ifndef CONCURRENTCACHE_BASE_SIGNAL_H
#define CONCURRENTCACHE_BASE_SIGNAL_H

#include <functional>
#include <unordered_map>
#include <csignal>
#include <string>
#include "log.h"

namespace cc_server {
    /**
     * @brief SignalHandler类：系统信号处理器
     *
     * 协作关系：
     * - 处理 SIGINT（Ctrl+C）和 SIGTERM 信号，实现优雅退出
     * - 使用单例模式，全局统一管理信号
     * - 信号触发时调用用户注册的回调函数
     */
    class SignalHandler {
    public:
        using SignalCallback = std::function<void()>;

        // 获取单例实例
        static SignalHandler& getInstance();

        // 注册信号处理函数
        void handle(int sigal_num, SignalCallback callback);

    private:
        SignalHandler();
        static void signalHandler(int signal_num);
        std::unordered_map<int, SignalCallback> callbacks_;
    };
}

#endif // CONCURRENTCACHE_BASE_SIGNAL_H