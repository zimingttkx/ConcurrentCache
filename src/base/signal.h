// 头文件保护宏 防止头文件重复包含

#ifndef CONCURRENTCACHE_BASE_SIGNAL_H
#define CONCURRENTCACHE_BASE_SIGNAL_H

#include <functional> // 用于function包装回调函数
#include <unordered_map> // 存储信号编号-处理函数的映射
#include <csignal> // C++11标准库中信号处理相关的头文件
#include <string> // 用于字符串处理 打印日志
#include "log.h"

namespace cc_server {
    //单例模式 全局一个信号处理器 不能创建多个对象
    class SignalHandler {
    public:
        // 定义一种无参数 无返回值的函数类型 名字叫SignalCallback
        // 用来存储型号触发的时候要执行的自定义函数
        // 作用：把「信号触发后要执行的代码」存起来，需要的时候直接调用
        using SignalCallback = std::function<void()>;

        // 静态成员函数 属于类本身 不需要创建对象也可以调用
        static SignalHandler& getInstance() {
            // 程序运行期间只创建一次
            static SignalHandler instance;
            return instance; //返回唯一单例对象
        }

        // 注册信号和对应的处理函数
        // 功能：告诉系统「收到XX信号，就执行XX函数」
        // 参数1：signal_num → 系统信号编号（比如 SIGINT=Ctrl+C、SIGTERM=程序关闭）
        // 参数2：callback → 信号触发时，你自己写的要执行的函数（无参无返回值）

        void handle(int sigal_num, SignalCallback callback) {
            // 将信号编号和自定义处理函数存储到hash表里面
            callbacks_[sigal_num] = callback;
            // 调用系统API 注册信号处理函数
            // 作用：操作系统收到 signal_num 信号时，自动调用 signalHandler 函数
            // 固定写法：系统要求信号处理函数必须是 静态函数/全局函数
            std::signal(sigal_num, SignalHandler::signalHandler);
        }
    private:
        // 构造函数私有化 禁止外部创建对象
        SignalHandler() = default;

        // 静态成员函数：
        // 1. 没有this指针，符合系统信号处理函数的格式要求
        // 2. 系统只能调用 静态/全局 函数，不能调用普通成员函数
        // 参数 signal_num：操作系统传过来的 → 触发的信号编号
        static void signalHandler(int signal_num) {
            // 获取单例对象 因为静态函数不能直接访问非静态成员变量本身
            auto& instance = getInstance();
            if (instance.callbacks_.find(signal_num) != instance.callbacks_.end()) {
                // 找到了 就执行用户注册的自定义回调函数
                instance.callbacks_[signal_num]();
            }else {
                // 没找到 就打印日志
                LOG_ERROR("收到未处理的信号:" + std::to_string(signal_num));
            }
        }
        // 存储信号和处理函数的映射
        std::unordered_map<int, SignalCallback> callbacks_;

    };
}

#endif // CONCURRENTCACHE_BASE_SIGNAL_H
