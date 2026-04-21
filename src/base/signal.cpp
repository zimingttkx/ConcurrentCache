#include "signal.h"

namespace cc_server {
    // 私有构造函数
    SignalHandler::SignalHandler() = default;

    // 获取单例实例
    SignalHandler& SignalHandler::getInstance() {
        static SignalHandler instance;
        return instance;
    }

    // 注册信号处理函数
    // - 将回调存入 hash 表
    // - 调用 std::signal() 注册系统信号处理函数
    void SignalHandler::handle(int signal_num, SignalCallback callback) {
        callbacks_[signal_num] = callback;
        std::signal(signal_num, SignalHandler::signalHandler);
    }

    // 系统信号处理函数（静态）
    // - 通过 getInstance() 获取单例
    // - 查找并执行对应的回调函数
    void SignalHandler::signalHandler(int signal_num) {
        auto& instance = getInstance();
        if (instance.callbacks_.find(signal_num) != instance.callbacks_.end()) {
            instance.callbacks_[signal_num]();
        } else {
            LOG_ERROR("收到未处理的信号: %d", signal_num);
        }
    }
}