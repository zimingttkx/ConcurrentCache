#include <iostream>
#include <csignal>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <sys/resource.h>
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

// 信号处理函数 — 必须是 async-signal-safe
// 只能做: atomic flag 设置, write(), _exit()
void signal_handler(int sig) {
    (void)sig; // 参数仅在回调签名中需要，未使用
    if (!g_running.exchange(false)) {
        return; // 已经在处理退出了
    }
    // write() 到 STDERR_FILENO 是 async-signal-safe
    const char* msg = "\n[信号处理] 收到信号，开始优雅退出...\n";
    [[maybe_unused]] ssize_t _unused = write(STDERR_FILENO, msg, strlen(msg));

    // EventLoop::quit() 只做 atomic store — 信号安全
    if (g_main_reactor) {
        g_main_reactor->event_loop()->quit();
    }
}

namespace {

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n"
              << "  --config <path>   配置文件路径（默认 conf/concurrentcache.conf）\n"
              << "  --port <n>        覆盖配置文件里的监听端口\n"
              << "  --help, -h        显示本帮助\n";
}

} // namespace

int main(int argc, char* argv[]) {
    const char* config_path = "conf/concurrentcache.conf";
    bool config_explicit = false;
    int port_override = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "[主线程] --config 缺少路径参数" << std::endl;
                return 1;
            }
            config_path = argv[++i];
            config_explicit = true;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "[主线程] --port 缺少端口参数" << std::endl;
                return 1;
            }
            const std::string value = argv[++i];
            try {
                size_t consumed = 0;
                const int parsed = std::stoi(value, &consumed);
                if (consumed != value.size() || parsed < 1 || parsed > 65535) {
                    std::cerr << "[主线程] 非法端口: " << value << std::endl;
                    return 1;
                }
                port_override = parsed;
            } catch (const std::exception&) {
                std::cerr << "[主线程] 非法端口: " << value << std::endl;
                return 1;
            }
        } else {
            std::cerr << "[主线程] 未知参数: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // 0. 提升文件描述符上限（高并发必须）
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max > 65535 ? 65535 : rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            std::cerr << "[主线程] 警告: 无法提升 fd 上限" << std::endl;
        }
    }
    std::cout << "========================================" << std::endl;
    std::cout << "   ConcurrentCache 高并发内存缓存服务器   " << std::endl;
    std::cout << "           Version 3.0 (RDB Persist)       " << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 注册信号处理器（必须在其他组件之前，防止初始化期间收到信号无法处理）
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    // 初始化内部信号系统（SIGPIPE忽略, SIGSEGV堆栈捕获）
    SignalHandler::getInstance().init();
    std::cout << "[主线程] 信号系统初始化完成" << std::endl;

    // 2. 加载配置文件
    if (!Config::instance().load(config_path)) {
        if (config_explicit) {
            // 用户点名了配置文件却读不到，静默退回默认值是错的
            std::cerr << "[主线程] 配置文件加载失败: " << config_path << std::endl;
            return 1;
        }
        // 默认路径不存在则继续：从 build/ 目录启动（没有 conf/）是受支持的用法
        std::cout << "[主线程] 未找到配置文件 " << config_path << "，使用默认配置" << std::endl;
    }
    std::cout << "[主线程] 配置系统初始化完成" << std::endl;

    // 3. 初始化日志系统
    // log_level 这个词法必须和热加载路径一致：Config 里存的是字符串，conf 写的是
    // 数字（log_level = 4），以前 main 用 getInt 读、onConfigChange 只认名字，
    // 两边读同一个键却互不兼容。
    LogLevel log_level = LogLevel::DEBUG;
    const std::string level_text = Config::instance().getString("log_level", "1");
    if (!parse_log_level(level_text, log_level)) {
        std::cerr << "[主线程] 无法识别的 log_level: \"" << level_text
                  << "\"，退回默认级别 DEBUG" << std::endl;
    }
    Logger::instance().setLevel(log_level);

    // Config 早就把 log_file 的默认值塞进配置表了，但 Logger::setFile() 从来没被
    // 调用过 —— 配置文件承诺写文件日志，实际只出控制台，setFile/rotate/cleanup
    // 一整条路径都是死代码。这里接上。
    const std::string log_file = Config::instance().getString("log_file", "");
    if (!log_file.empty()) {
        const long long max_bytes = Config::instance().getInt("log_max_size", 104857600);
        Logger::instance().setRotation(max_bytes > 0 ? static_cast<size_t>(max_bytes) : 104857600, 5);
        Logger::instance().setFile(log_file);
    }
    std::cout << "[主线程] 日志系统初始化完成 (级别: " << level_text << ", 输出: "
              << (log_file.empty() ? "仅控制台" : log_file) << ")" << std::endl;

    // 4. 获取配置参数（默认使用 CPU 核心数）
    int port = Config::instance().getInt("port", 6379);
    if (port_override > 0) {
        port = port_override;
    }
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
    SubReactorPool::instance().init(static_cast<size_t>(reactor_count));
    g_sub_reactor_pool = &SubReactorPool::instance();
    std::cout << "[主线程] SubReactorPool 初始化完成" << std::endl;

    // 6. 初始化通用线程池（用于异步任务处理）
    ThreadPool thread_pool(static_cast<size_t>(thread_pool_size));
    std::cout << "[主线程] 通用线程池创建完成 (" << thread_pool_size << " 工作线程)" << std::endl;

    // 7. 启动 SubReactorPool
    SubReactorPool::instance().start();
    std::cout << "[主线程] SubReactorPool 启动完成 (" << reactor_count << " 个 SubReactor)" << std::endl;

    // 8. 初始化 MainReactor（单线程处理 accept）
    MainReactor main_reactor;
    if (!main_reactor.init(port)) {
        // 监听失败必须让进程以非 0 退出：旧代码只把 g_running 置 false，然后
        // 照样启动过期检查器、RDB 调度器和集群，最后 return 0 —— systemd/docker
        // 看到的是“进程健康”，实际一个客户端都连不上。
        std::cerr << "[主线程] MainReactor 初始化失败，监听端口 " << port
                  << " 可能已被占用" << std::endl;
        SubReactorPool::instance().stop();
        return 1;
    }
    g_main_reactor = &main_reactor;
    std::cout << "[主线程] MainReactor 初始化完成，监听端口 " << port << std::endl;

    // 9. 加载 RDB 持久化文件（必须在 ExpirationChecker 启动之前，避免并发访问）
    auto& rdb = RdbPersistence::instance();
    std::string rdb_path = Config::instance().getString("rdb_path", "./dump.rdb");
    if (access(rdb_path.c_str(), R_OK) != 0) {
        std::cout << "[主线程] 没有可读的 RDB 文件，将从空存储开始" << std::endl;
    } else if (rdb.load(rdb_path, GlobalStorage::instance())) {
        std::cout << "[主线程] RDB 数据加载成功" << std::endl;
    } else {
        // 文件在、却读不出来 = 数据损坏。继续启动就是拿一个残缺数据集对外服务，
        // 而且客户端再也分不清"这就是库里全部内容"和"启动时把文件读崩了"。
        // 这种情况必须让退出码说话，systemd / docker 才看得到。
        std::cerr << "[主线程] RDB 文件存在但加载失败：" << rdb_path
                  << "，拒绝启动以免对外提供残缺数据" << std::endl;
        SubReactorPool::instance().stop();
        return 1;
    }

    // 9.1 设置集群的 EventLoop（在集群初始化之前）
    if (g_main_reactor) {
        ClusterServer::instance().set_event_loop(g_main_reactor->event_loop());
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

    // 走到这里说明已经在监听；g_running 为 false 只可能是启动期间就收到退出信号
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

    // 15. 优雅退出流程
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

    // 停止过期键检查器
    expiration_checker.stop();
    std::cout << "[主线程] 过期键检查器已停止" << std::endl;

    // 停止集群
    ClusterServer::instance().stop();
    std::cout << "[主线程] 集群已停止" << std::endl;

    // 停止线程池（必须最后停止，其他组件可能有任务在队列中）
    thread_pool.stop();
    std::cout << "[主线程] 线程池已停止" << std::endl;

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