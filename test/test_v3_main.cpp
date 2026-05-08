#include <iostream>
#include <string>

namespace cc_server {
namespace testing {

// 声明所有测试函数
void run_all_datatype_tests();
void run_all_rdb_tests();
void run_all_storage_tests();
// void run_all_command_tests();  // 暂时禁用

}  // namespace testing
}  // namespace cc_server

int main(int argc, char* argv[]) {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "ConcurrentCache V3 Test Suite\n";
    std::cout << "========================================\n";
    std::cout << "\n";

    bool run_all = true;
    bool run_datatype = false;
    bool run_rdb = false;
    bool run_storage = false;
    // bool run_command = false;  // 暂时禁用

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--datatype") {
            run_datatype = true;
            run_all = false;
        } else if (arg == "--rdb") {
            run_rdb = true;
            run_all = false;
        } else if (arg == "--storage") {
            run_storage = true;
            run_all = false;
        // } else if (arg == "--command") {  // 暂时禁用
        //     run_command = true;
        //     run_all = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "\nOptions:\n";
            std::cout << "  --datatype    Run CacheObject datatype tests only\n";
            std::cout << "  --rdb         Run RDB persistence tests only\n";
            std::cout << "  --storage     Run GlobalStorage V3 tests only\n";
            std::cout << "  --command     Run command layer tests only\n";
            std::cout << "  --help, -h    Show this help message\n";
            std::cout << "\nIf no options are specified, all tests will run.\n";
            return 0;
        }
    }

    try {
        if (run_all || run_datatype) {
            cc_server::testing::run_all_datatype_tests();
        }

        if (run_all || run_rdb) {
            cc_server::testing::run_all_rdb_tests();
        }

        if (run_all || run_storage) {
            cc_server::testing::run_all_storage_tests();
        }

        // if (run_all || run_command) {  // 暂时禁用
        //     cc_server::testing::run_all_command_tests();
        // }

        std::cout << "\n";
        std::cout << "========================================\n";
        std::cout << "✓ All V3 Tests Passed Successfully!\n";
        std::cout << "========================================\n";
        std::cout << "\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n";
        std::cerr << "========================================\n";
        std::cerr << "✗ Test Failed with Exception:\n";
        std::cerr << e.what() << "\n";
        std::cerr << "========================================\n";
        std::cerr << "\n";
        return 1;
    }
}
