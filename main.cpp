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
#include "src/cache/storage.h"
#include "src/cache/expiration_checker.h"
#include "src/persistence/rdb.h"
#include "src/persistence/rdb_scheduler.h"
#include "src/cluster/cluster_server.h"

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
    std::cout << "           Version 3.0 (RDB Persist)       " << std::endl;
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

    // 4. 获取配置参数（默认使用 CPU 核心数）
    int port = Config::instance().getInt("port", 6379);
    int reactor_count = Config::instance().getInt("reactor_count",
        static_cast<int>(std::thread::hardware_concurrency()));
    int thread_pool_size = Config::instance().getInt("thread_pool_size",
        static_cast<int>(std::thread::hardware_concurrency()));
    int rdb_save_interval = Config::instance().getInt("rdb_save_interval", 900);
    int rdb_dirty_threshold = Config::instance().getInt("rdb_dirty_threshold", 1);

    // 确保至少有一个线程
    if (reactor_count <= 0) reactor_count = 1;
    if (thread_pool_size <= 0) thread_pool_size = 1;
    if (rdb_save_interval <= 0) rdb_save_interval = 900;
    if (rdb_dirty_threshold <= 0) rdb_dirty_threshold = 1;

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

    // 10. 加载 RDB 持久化文件（必须在 ExpirationChecker 启动之前，避免并发访问）
    auto& rdb = RdbPersistence::instance();
    std::string rdb_path = Config::instance().getString("rdb_path", "./dump.rdb");
    if (rdb.load(rdb_path, GlobalStorage::instance())) {
        std::cout << "[主线程] RDB 数据加载成功" << std::endl;
    } else {
        std::cout << "[主线程] 无 RDB 文件或加载失败，将从空存储开始" << std::endl;
    }

    // 10. 初始化集群（如果启用）
    ClusterServer::instance().init();
    if (ClusterServer::instance().isEnabled()) {
        std::cout << "[主线程] 集群模块初始化完成" << std::endl;
    }

    // 11. 启动过期键检查器（后台线程定期清理过期键）
    ExpirationChecker expiration_checker(GlobalStorage::instance().expire_dict(), GlobalStorage::instance());
    expiration_checker.start();
    std::cout << "[主线程] 过期键检查器已启动" << std::endl;

    // 12. 启动 RDB 自动保存调度器
    RdbScheduler rdb_scheduler(GlobalStorage::instance(), rdb_path);
    RdbScheduler::SaveConfig scheduler_config;
    scheduler_config.interval_sec = rdb_save_interval;
    scheduler_config.dirty_threshold = rdb_dirty_threshold;
    rdb_scheduler.set_config(scheduler_config);
    rdb_scheduler.start();
    std::cout << "[主线程] RDB 自动保存调度器已启动 (间隔: " << rdb_save_interval << "s, 脏键阈值: " << rdb_dirty_threshold << ")" << std::endl;

    // 只有 MainReactor 初始化成功时才显示服务器启动信息
    if (g_running) {
        std::cout << "========================================" << std::endl;
        std::cout << "   服务器启动成功！等待客户端连接...     " << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "   架构特性:                            " << std::endl;
        std::cout << "   - MainReactor: 单线程处理 accept     " << std::endl;
        std::cout << "   - SubReactorPool: " << reactor_count << " 线程处理 I/O      " << std::endl;
        std::cout << "   - ThreadPool: " << thread_pool_size << " 线程处理异步任务  " << std::endl;
        std::cout << "   - RDB Persist: 自动持久化支持        " << std::endl;
        std::cout << "========================================" << std::endl;
    } else {
        std::cout << "========================================" << std::endl;
        std::cout << "   服务器启动失败！                     " << std::endl;
        std::cout << "========================================" << std::endl;
    }

    // 13. 启动集群（如果启用）
    ClusterServer::instance().start();

    // 14. 启动 MainReactor 事件循环（阻塞）
    if (g_running) {
        main_reactor.start();
    }

    // 14. 优雅退出流程
    std::cout << "\n[主线程] 开始关闭服务器..." << std::endl;

    // 停止 RDB 自动保存调度器
    rdb_scheduler.stop();
    std::cout << "[主线程] RDB 调度器已停止" << std::endl;

    // 停止 SubReactorPool
    SubReactorPool::instance().stop();
    std::cout << "[主线程] SubReactorPool 已停止" << std::endl;

    // 停止 MainReactor
    main_reactor.stop();
    std::cout << "[主线程] MainReactor 已停止" << std::endl;

    // 停止线程池
    thread_pool.stop();
    std::cout << "[主线程] 线程池已停止" << std::endl;

    // 停止过期键检查器
    expiration_checker.stop();
    std::cout << "[主线程] 过期键检查器已停止" << std::endl;

    // 停止集群
    ClusterServer::instance().stop();
    std::cout << "[主线程] 集群已停止" << std::endl;

    // 15. 保存 RDB 持久化文件（优雅退出时自动保存）
    if (rdb.save(rdb_path, GlobalStorage::instance())) {
        std::cout << "[主线程] RDB 数据保存成功" << std::endl;
    } else {
        std::cout << "[主线程] RDB 数据保存失败" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "   服务器已安全退出，感谢使用！         " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}