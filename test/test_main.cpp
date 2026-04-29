#include <iostream>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <thread>

// 测试函数声明
namespace cc_server {
namespace testing {

void run_thread_pool_tests();
void run_lock_tests();
void run_memory_pool_tests();

} // namespace testing
} // namespace cc_server

using namespace cc_server::testing;

void print_header(const std::string& title) {
    std::cout << "\n";
    std::cout << "########################################\n";
    std::cout << "#  " << title << "\n";
    std::cout << "########################################\n";
}

int main(int argc, char* argv[]) {
    // 初始化随机种子
    std::srand(static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count()));

    print_header("ConcurrentCache Concurrency Test Suite");

    std::cout << "Test start time: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
    std::cout << "Hardware concurrency: " << std::thread::hardware_concurrency() << "\n";

    bool run_thread_pool = true;
    bool run_lock = true;
    bool run_memory_pool = true;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --thread-pool    Run thread pool tests\n";
            std::cout << "  --lock           Run lock tests\n";
            std::cout << "  --memory-pool    Run memory pool tests\n";
            std::cout << "  --all            Run all tests (default)\n";
            std::cout << "  --help, -h       Show this help\n";
            return 0;
        } else if (arg == "--thread-pool") {
            run_lock = false;
            run_memory_pool = false;
        } else if (arg == "--lock") {
            run_thread_pool = false;
            run_memory_pool = false;
        } else if (arg == "--memory-pool") {
            run_thread_pool = false;
            run_lock = false;
        }
    }

    int exit_code = 0;

    try {
        if (run_thread_pool) {
            run_thread_pool_tests();
        }

        if (run_lock) {
            run_lock_tests();
        }

        if (run_memory_pool) {
            run_memory_pool_tests();
        }

        print_header("Test Summary");
        std::cout << "All tests completed.\n";
        std::cout << "Check the logs/ directory for detailed trace files.\n";

    } catch (const std::exception& e) {
        std::cerr << "Exception caught: " << e.what() << "\n";
        exit_code = 1;
    } catch (...) {
        std::cerr << "Unknown exception caught\n";
        exit_code = 1;
    }

    return exit_code;
}
