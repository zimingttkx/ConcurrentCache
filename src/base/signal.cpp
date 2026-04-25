#include "signal.h"
#include <execinfo.h>      // backtrace() 系列函数：获取调用栈
#include <dlfcn.h>         // dladdr()：获取符号信息（函数名、文件名）
#include <cxxabi.h>        // abi::__cxa_demangle：C++ 名字修饰还原
#include <unistd.h>        // _exit()：紧急退出（不能使用 exit，信号处理中不安全）

namespace cc_server {

    // ==================== 构造函数 ====================

    SignalHandler::SignalHandler() = default;

    // ==================== 单例获取 ====================

    SignalHandler& SignalHandler::getInstance() {
        // C++11 保证静态局部变量线程安全
        static SignalHandler instance;
        return instance;
    }

    // ==================== V1 已有功能 ====================

    /**
     * 注册信号处理函数
     *
     * 原理：
     * 1. 将回调存入 hash 表（便于后续查找）
     * 2. 调用 std::signal() 注册系统信号处理函数
     *
     * 注意：同一个信号多次注册会覆盖旧回调
     */
    void SignalHandler::handle(int signal_num, SignalCallback callback) {
        callbacks_[signal_num] = callback;
        std::signal(signal_num, SignalHandler::signalHandler);
    }

    /**
     * 信号处理桥梁函数（静态）
     *
     * 被系统调用 → 查找并执行对应的 C++ 回调
     * 这是 C 风格信号机制和 C++ 回调之间的桥梁
     */
    void SignalHandler::signalHandler(int signal_num) {
        auto& instance = getInstance();

        // 查找并执行注册的回调
        if (instance.callbacks_.find(signal_num) != instance.callbacks_.end()) {
            instance.callbacks_[signal_num]();
        } else {
            // 没有注册回调，记录日志后忽略
            LOG_ERROR(signal, "收到未处理的信号: %d", signal_num);
        }
    }

    // ==================== V2 新增功能 ====================

    /**
     * 初始化信号系统（V2 新增）
     *
     * 在 main() 最开始调用，设置好信号处理
     *
     * 处理内容：
     * 1. SIGPIPE → 忽略，防止写已关闭连接导致崩溃
     *    - 当对端关闭连接，你还在写数据，操作系统发 SIGPIPE
     *    - 默认行为是终止进程，我们需要忽略它，让 write() 返回 EPIPE
     *
     * 2. SIGSEGV → 捕获，打印堆栈
     *    - 段错误（访问非法内存）发生时
     *    - 打印函数调用栈，便于定位问题
     */
    void SignalHandler::init() {
        // 1. 忽略 SIGPIPE
        //
        // 为什么要忽略？
        // 假设：客户端异常断开，但服务器还在往连接里写数据
        // 结果：操作系统发 SIGPIPE → 默认行为是 kill 进程
        // 忽略后：write() 会返回 -1，errno = EPIPE，我们可以从容处理
        //
        // SIG_IGN = 忽略该信号
        if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
            // 如果设置失败，记录日志（但程序继续运行）
            fprintf(stderr, "警告: 无法忽略 SIGPIPE 信号\n");
        }

        // 2. 注册 SIGSEGV 捕获
        //
        // 为什么要捕获？
        // 段错误发生时会收到 SIGSEGV（硬件异常，非软件异常）
        // 如果不捕获：进程直接崩溃，无任何信息
        // 捕获后：打印堆栈，记录日志，然后优雅退出
        if (signal(SIGSEGV, SignalHandler::sigsegvHandler) == SIG_ERR) {
            fprintf(stderr, "警告: 无法注册 SIGSEGV 处理器\n");
        }

        LOG_INFO(signal, "信号系统初始化完成: SIGPIPE=忽略, SIGSEGV=堆栈捕获");
    }

    /**
     * 获取堆栈跟踪信息（V2 新增）
     *
     * 使用 glibc 的 backtrace() 系列函数获取调用栈
     *
     * @return 堆栈帧列表，每帧格式如：
     *   "#0  0x7f9a3c2d4a1c  __GI_raise"
     *   "#1  0x7f9a3c2d4891  __GI_abort"
     *   "#2  0x401234  GlobalStorage::get(string const&)"
     *
     * 实现原理：
     * 1. backtrace() 获取原始地址列表
     * 2. backtrace_symbols() 转换地址为符号（但格式简陋）
     * 3. dladdr() + abi::__cxa_demangle 还原 C++ 函数名
     */
    std::vector<std::string> SignalHandler::getStackTrace() {
        std::vector<std::string> stack;

        // 预留空间，避免多次分配
        // MAX_STACK_DEPTH = 64 通常足够深
        constexpr size_t MAX_STACK_DEPTH = 64;
        void* buffer[MAX_STACK_DEPTH];

        // 第一步：获取原始堆栈地址
        // 返回值 = 实际获取的帧数
        int size = backtrace(buffer, MAX_STACK_DEPTH);

        if (size <= 0) {
            stack.push_back("  (无堆栈信息: backtrace 失败)");
            return stack;
        }

        // 第二步：逐帧解析
        // 从 index = 1 开始，跳过当前函数本身
        for (int i = 1; i < size; i++) {
            void* addr = buffer[i];

            // 使用 dladdr() 获取地址对应的符号信息
            // Dl_info 包含：dli_fname（文件）、dli_sname（符号名）、dli_saddr（地址）
            Dl_info info;
            if (dladdr(addr, &info) != 0) {
                std::string frame = "  #" + std::to_string(i - 1) + "  ";

                // 1) 添加地址
                char addr_buf[32];
                snprintf(addr_buf, sizeof(addr_buf), "%p", addr);
                frame += std::string(addr_buf) + "  ";

                // 2) 添加函数名（如果存在）
                if (info.dli_sname) {
                    // 检查是否是 C++ 修饰过的名字（如 _Z13GlobalStorage3getRKSs）
                    int status = 0;
                    char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);

                    if (status == 0 && demangled) {
                        // 还原成功，使用原始名字
                        frame += demangled;
                        free(demangled);  // __cxa_demangle 返回的需要 free
                    } else {
                        // 还原失败（可能是纯 C 函数），使用原始名字
                        frame += info.dli_sname;
                    }
                } else {
                    // 没有符号名，只显示地址
                    frame += "(未知符号)";
                }

                // 3) 添加偏移量（相对符号起始地址）
                if (info.dli_saddr) {
                    char offset_buf[64];
                    snprintf(offset_buf, sizeof(offset_buf), " + %ld",
                             (char*)addr - (char*)info.dli_saddr);
                    frame += offset_buf;
                }

                stack.push_back(frame);
            } else {
                // dladdr 失败，只显示地址
                char addr_buf[32];
                snprintf(addr_buf, sizeof(addr_buf), "%p", addr);
                stack.push_back("  #" + std::to_string(i - 1) + "  " + addr_buf + "  (无符号信息)");
            }
        }

        return stack;
    }

    /**
     * 打印堆栈到日志（V2 新增）
     *
     * 段错误发生时调用，打印完整调用栈
     * 使用 LOG_ERROR 输出，保证能记录到日志文件
     *
     * 注意：
     * - 不能使用 exit()，信号处理中不安全
     * - 使用 _exit(1) 直接终止进程
     */
    void SignalHandler::printStackTrace() {
        // 1. 打印分隔线
        LOG_ERROR(signal, "========================================");
        LOG_ERROR(signal, "SIGSEGV 捕获！正在打印堆栈...");
        LOG_ERROR(signal, "========================================");

        // 2. 获取并打印堆栈
        auto stack = getStackTrace();
        for (const auto& frame : stack) {
            LOG_ERROR(signal, "%s", frame.c_str());
        }

        // 3. 打印分隔线
        LOG_ERROR(signal, "========================================");
        LOG_ERROR(signal, "进程即将终止，请检查上方堆栈定位问题");
        LOG_ERROR(signal, "========================================");

        // 4. 紧急退出
        //
        // 为什么用 _exit() 而不是 exit()？
        // - exit() 会调用 atexit() 回调、刷新缓冲区、关闭文件描述符
        //   这些操作在信号处理上下文中可能引发新的信号，导致死锁
        // - _exit() 直接终止进程，不做任何清理
        //   在这里我们是紧急情况，不需要优雅退出
        _exit(1);
    }

    /**
     * SIGSEGV 处理函数（V2 新增，静态）
     *
     * 段错误（SIGSEGV）发生时，系统会调用这个函数
     * 打印堆栈后直接退出
     *
     * 注意：
     * - 这是信号处理函数，必须是 async-signal-safe 的
     * - 不能使用 LOG_INFO、LOG_DEBUG 等（可能加锁，非安全）
     * - 只能使用 async-signal-safe 的函数
     *
     * 为什么使用 fprintf + _exit？
     * - fprintf(stderr) 是 async-signal-safe 的
     * - _exit() 是 async-signal-safe 的
     * - LOG_ERROR 底层可能加锁，不安全
     */
    void SignalHandler::sigsegvHandler(int signal_num) {
        // 打印基本信息
        fprintf(stderr, "\n");
        fprintf(stderr, "========================================\n");
        fprintf(stderr, "SIGSEGV 捕获！信号编号: %d\n", signal_num);
        fprintf(stderr, "========================================\n");

        // 获取堆栈
        constexpr size_t MAX_STACK_DEPTH = 64;
        void* buffer[MAX_STACK_DEPTH];
        int size = backtrace(buffer, MAX_STACK_DEPTH);

        if (size > 0) {
            fprintf(stderr, "调用堆栈:\n");

            // backtrace_symbols_fd 直接输出到 fd，不需要额外的字符串处理
            // 但输出格式比较简陋，所以我们还是用 backtrace_symbols
            char** symbols = backtrace_symbols(buffer, size);
            if (symbols) {
                // 从 index = 1 开始，跳过当前函数
                for (int i = 1; i < size; i++) {
                    fprintf(stderr, "  #%d  %s\n", i - 1, symbols[i]);
                }
                free(symbols);
            } else {
                // fallback：只用地址
                for (int i = 1; i < size; i++) {
                    fprintf(stderr, "  #%d  %p\n", i - 1, buffer[i]);
                }
            }
        } else {
            fprintf(stderr, "  (无堆栈信息)\n");
        }

        fprintf(stderr, "========================================\n");
        fprintf(stderr, "进程即将终止\n");
        fprintf(stderr, "========================================\n");

        // 紧急退出
        _exit(1);
    }

}  // namespace cc_server
