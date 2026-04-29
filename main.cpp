#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include "src/base/log.h"
#include "src/base/config.h"
#include "src/base/signal.h"
#include "src/base/thread_pool.h"
#include "src/network/main_reactor.h"
#include "src/network/sub_reactor_pool.h"

using namespace cc_server;

// 全局停止标志
std::atomic<bool> g_running{true};

// 全局组件指针（用于信号处理）
SubReactorPool* g_sub_reactor_pool = nullptr;
MainReactor* g_main_reactor = nullptr;

// 信号处理函数
void signal_handler(int sig) {
    if (!g_running.exchange(false)) {
        return; // 已经在处理退出了
    }
    std::cout << "\n[信号处理] 收到信号 " << sig << "，开始优雅退出..." << std::endl;

    // 唤醒 MainReactor 让它退出
    if (g_main_reactor) {
        g_main_reactor->event_loop()->quit();
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   ConcurrentCache 高并发内存缓存服务器   " << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 初始化信号系统（最先初始化，处理 SIGINT/SIGTERM 实现优雅退出）
    SignalHandler::getInstance().init();
    std::cout << "[主线程] 信号系统初始化完成" << std::endl;

    // 2. 加载配置文件
    if (!Config::instance().load("conf/concurrentcache.conf")) {
        std::cerr << "[主线程] 配置文件加载失败，使用默认配置" << std::endl;
    }
    std::cout << "[主线程] 配置系统初始化完成" << std::endl;

    // 3. 初始化日志系统
    int log_level = Config::instance().getInt("log_level", 1);
    Logger::instance().setLevel(static_cast<LogLevel>(log_level));
    std::cout << "[主线程] 日志系统初始化完成 (级别: " << log_level << ")" << std::endl;

    // 4. 获取配置参数
    int port = Config::instance().getInt("port", 6379);
    int reactor_count = Config::instance().getInt("reactor_count", 4);
    int thread_pool_size = Config::instance().getInt("thread_pool_size", 4);

    std::cout << "[主线程] 监听端口: " << port << std::endl;
    std::cout << "[主线程] SubReactor 数量: " << reactor_count << std::endl;
    std::cout << "[主线程] 线程池大小: " << thread_pool_size << std::endl;

    // 5. 初始化 SubReactorPool（多线程处理 I/O 事件）
    // 必须先于 ThreadPool 初始化，因为 MainReactor 会用到
    SubReactorPool::instance().init(reactor_count);
    g_sub_reactor_pool = &SubReactorPool::instance();
    std::cout << "[主线程] SubReactorPool 初始化完成" << std::endl;

    // 6. 初始化通用线程池（用于异步任务处理）
    static ThreadPool thread_pool(thread_pool_size);
    std::cout << "[主线程] 通用线程池创建完成 (" << thread_pool_size << " 工作线程)" << std::endl;

    // 7. 启动 SubReactorPool
    SubReactorPool::instance().start();
    std::cout << "[主线程] SubReactorPool 启动完成 (" << reactor_count << " 个 SubReactor)" << std::endl;

    // 8. 初始化 MainReactor（单线程处理 accept）
    MainReactor main_reactor;
    if (!main_reactor.init(port)) {
        std::cerr << "[主线程] MainReactor 初始化失败" << std::endl;
        g_running = false;
    } else {
        g_main_reactor = &main_reactor;
        std::cout << "[主线程] MainReactor 初始化完成，监听端口 " << port << std::endl;
    }

    // 9. 注册信号处理回调（使用自定义信号处理函数）
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 只有 MainReactor 初始化成功时才显示服务器启动信息
    if (g_running) {
        std::cout << "========================================" << std::endl;
        std::cout << "   服务器启动成功！等待客户端连接...     " << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "   架构特性:                            " << std::endl;
        std::cout << "   - MainReactor: 单线程处理 accept     " << std::endl;
        std::cout << "   - SubReactorPool: " << reactor_count << " 线程处理 I/O      " << std::endl;
        std::cout << "   - ThreadPool: " << thread_pool_size << " 线程处理异步任务  " << std::endl;
        std::cout << "========================================" << std::endl;
    } else {
        std::cout << "========================================" << std::endl;
        std::cout << "   服务器启动失败！                     " << std::endl;
        std::cout << "========================================" << std::endl;
    }

    // 10. 启动 MainReactor 事件循环（阻塞）
    if (g_running) {
        main_reactor.start();
    }

    // 11. 优雅退出流程
    std::cout << "\n[主线程] 开始关闭服务器..." << std::endl;

    // 停止 SubReactorPool
    SubReactorPool::instance().stop();
    std::cout << "[主线程] SubReactorPool 已停止" << std::endl;

    // 停止 MainReactor
    main_reactor.stop();
    std::cout << "[主线程] MainReactor 已停止" << std::endl;

    // 停止线程池
    thread_pool.stop();
    std::cout << "[主线程] 线程池已停止" << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "   服务器已安全退出，感谢使用！         " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
