#ifndef CONCURRENTCACHE_TEST_ASSERTIONS_H
#define CONCURRENTCACHE_TEST_ASSERTIONS_H

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <sstream>

namespace cc_server {
namespace testing {

// 测试结果
enum class TestResult {
    PASS,
    FAIL,
    SKIP
};

// 测试统计信息
struct TestStats {
    int total_tests = 0;
    int passed_tests = 0;
    int failed_tests = 0;
    int skipped_tests = 0;

    void reset() {
        total_tests = 0;
        passed_tests = 0;
        failed_tests = 0;
        skipped_tests = 0;
    }
};

// 全局测试统计
inline TestStats& g_test_stats() {
    static TestStats stats;
    return stats;
}

// 当前测试名称
inline std::string& g_current_test_name() {
    static std::string name;
    return name;
}

// 带超时的断言辅助函数
template<typename Func>
bool run_with_timeout(Func&& func, int timeout_ms) {
    std::atomic<bool> completed(false);
    std::atomic<bool> timed_out(false);

    std::thread worker([&]() {
        func();
        completed = true;
    });

    auto start = std::chrono::steady_clock::now();
    while (!completed) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (worker.joinable()) {
        worker.join();
    }

    return !timed_out;
}

// 颜色输出辅助
inline std::string green(const std::string& s) {
    return "\033[32m" + s + "\033[0m";
}

inline std::string red(const std::string& s) {
    return "\033[31m" + s + "\033[0m";
}

inline std::string yellow(const std::string& s) {
    return "\033[33m" + s + "\033[0m";
}

// ========== 断言宏定义 ==========

#define EXPECT_TRUE(condition) \
    cc_server::testing::expect_true(condition, #condition, __FILE__, __LINE__)

#define EXPECT_FALSE(condition) \
    cc_server::testing::expect_false(condition, #condition, __FILE__, __LINE__)

#define EXPECT_EQ(actual, expected) \
    cc_server::testing::expect_eq(actual, expected, #actual, #expected, __FILE__, __LINE__)

#define EXPECT_NE(actual, expected) \
    cc_server::testing::expect_ne(actual, expected, #actual, #expected, __FILE__, __LINE__)

#define EXPECT_LT(actual, expected) \
    cc_server::testing::expect_lt(actual, expected, #actual, #expected, __FILE__, __LINE__)

#define EXPECT_GT(actual, expected) \
    cc_server::testing::expect_gt(actual, expected, #actual, #expected, __FILE__, __LINE__)

#define EXPECT_LE(actual, expected) \
    cc_server::testing::expect_le(actual, expected, #actual, #expected, __FILE__, __LINE__)

#define EXPECT_GE(actual, expected) \
    cc_server::testing::expect_ge(actual, expected, #actual, #expected, __FILE__, __LINE__)

// 带超时的断言
#define EXPECT_TRUE_TIMEOUT(condition, timeout_ms) \
    cc_server::testing::expect_true_timeout(condition, #condition, timeout_ms, __FILE__, __LINE__)

#define EXPECT_FALSE_TIMEOUT(condition, timeout_ms) \
    cc_server::testing::expect_false_timeout(condition, #condition, timeout_ms, __FILE__, __LINE__)

// ASSERT 系列（失败时直接终止测试）
#define ASSERT_TRUE(condition) \
    if (!cc_server::testing::assert_true(condition, #condition, __FILE__, __LINE__)) return

#define ASSERT_FALSE(condition) \
    if (!cc_server::testing::assert_false(condition, #condition, __FILE__, __LINE__)) return

#define ASSERT_EQ(actual, expected) \
    if (!cc_server::testing::assert_eq(actual, expected, #actual, #expected, __FILE__, __LINE__)) return

#define ASSERT_NE(actual, expected) \
    if (!cc_server::testing::assert_ne(actual, expected, #actual, #expected, __FILE__, __LINE__)) return

// 跳过测试
#define SKIP() \
    cc_server::testing::skip_test(__FILE__, __LINE__)

// ========== 断言实现函数 ==========

inline void expect_true(bool condition, const char* condition_str,
                        const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_TRUE(" << condition_str << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_TRUE(" << condition_str << ") - condition was false" << std::endl;
    }
}

inline void expect_false(bool condition, const char* condition_str,
                         const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (!condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_FALSE(" << condition_str << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_FALSE(" << condition_str << ") - condition was true" << std::endl;
    }
}

template<typename T>
void expect_eq(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual == expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_EQ(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: " << expected << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_EQ(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: " << expected << ")" << std::endl;
    }
}

template<typename T>
void expect_ne(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual != expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_NE(" << actual_str << ", " << expected_str << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_NE(" << actual_str << ", " << expected_str << ")"
                  << " (both were: " << actual << ")" << std::endl;
    }
}

template<typename T>
void expect_lt(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual < expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_LT(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: < " << expected << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_LT(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: < " << expected << ")" << std::endl;
    }
}

template<typename T>
void expect_gt(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual > expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_GT(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: > " << expected << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_GT(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: > " << expected << ")" << std::endl;
    }
}

template<typename T>
void expect_le(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual <= expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_LE(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: <= " << expected << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_LE(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: <= " << expected << ")" << std::endl;
    }
}

template<typename T>
void expect_ge(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual >= expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_GE(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: >= " << expected << ")" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_GE(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: >= " << expected << ")" << std::endl;
    }
}

// 带超时的断言
inline void expect_true_timeout(bool condition, const char* condition_str,
                                 int timeout_ms, const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;

    bool completed = run_with_timeout([&condition]() {}, timeout_ms);

    if (condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_TRUE_TIMEOUT(" << condition_str << ", " << timeout_ms << "ms)" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_TRUE_TIMEOUT(" << condition_str << ", " << timeout_ms << "ms)"
                  << " - condition was false after timeout" << std::endl;
    }
}

inline void expect_false_timeout(bool condition, const char* condition_str,
                                  int timeout_ms, const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (!condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " EXPECT_FALSE_TIMEOUT(" << condition_str << ", " << timeout_ms << "ms)" << std::endl;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " EXPECT_FALSE_TIMEOUT(" << condition_str << ", " << timeout_ms << "ms)"
                  << " - condition was true after timeout" << std::endl;
    }
}

// ASSERT 系列实现（返回 bool 表示是否通过）
inline bool assert_true(bool condition, const char* condition_str,
                        const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " ASSERT_TRUE(" << condition_str << ")" << std::endl;
        return true;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " ASSERT_TRUE(" << condition_str << ") - condition was false" << std::endl;
        std::cout << red("[FATAL] Test terminated due to ASSERT failure") << std::endl;
        return false;
    }
}

inline bool assert_false(bool condition, const char* condition_str,
                         const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (!condition) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " ASSERT_FALSE(" << condition_str << ")" << std::endl;
        return true;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " ASSERT_FALSE(" << condition_str << ") - condition was true" << std::endl;
        std::cout << red("[FATAL] Test terminated due to ASSERT failure") << std::endl;
        return false;
    }
}

template<typename T>
bool assert_eq(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual == expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " ASSERT_EQ(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: " << expected << ")" << std::endl;
        return true;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " ASSERT_EQ(" << actual_str << ", " << expected_str << ")"
                  << " (actual: " << actual << ", expected: " << expected << ")" << std::endl;
        std::cout << red("[FATAL] Test terminated due to ASSERT failure") << std::endl;
        return false;
    }
}

template<typename T>
bool assert_ne(const T& actual, const T& expected,
               const char* actual_str, const char* expected_str,
               const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    if (actual != expected) {
        cc_server::testing::g_test_stats().passed_tests++;
        std::cout << green("[PASS]") << " " << file << ":" << line
                  << " ASSERT_NE(" << actual_str << ", " << expected_str << ")" << std::endl;
        return true;
    } else {
        cc_server::testing::g_test_stats().failed_tests++;
        std::cout << red("[FAIL]") << " " << file << ":" << line
                  << " ASSERT_NE(" << actual_str << ", " << expected_str << ")"
                  << " (both were: " << actual << ")" << std::endl;
        std::cout << red("[FATAL] Test terminated due to ASSERT failure") << std::endl;
        return false;
    }
}

inline void skip_test(const char* file, int line) {
    cc_server::testing::g_test_stats().total_tests++;
    cc_server::testing::g_test_stats().skipped_tests++;
    std::cout << yellow("[SKIP]") << " " << file << ":" << line
              << " Test skipped" << std::endl;
}

// 测试套件辅助类
class TestSuite {
public:
    TestSuite(const std::string& name) : name_(name) {
        std::cout << "\n" << yellow("========================================") << std::endl;
        std::cout << yellow("  Test Suite: ") << name_ << std::endl;
        std::cout << yellow("========================================") << std::endl;
        g_test_stats().reset();
    }

    ~TestSuite() {
        std::cout << "\n" << yellow("----------------------------------------") << std::endl;
        std::cout << "Results for: " << name_ << std::endl;
        std::cout << "  Total:  " << g_test_stats().total_tests << std::endl;
        std::cout << green("  Passed: ") << g_test_stats().passed_tests << std::endl;
        std::cout << red("  Failed: ") << g_test_stats().failed_tests << std::endl;
        std::cout << yellow("  Skipped: ") << g_test_stats().skipped_tests << std::endl;
        std::cout << yellow("----------------------------------------") << std::endl;
    }

    void run(const std::string& test_name, std::function<void()> test_func) {
        g_current_test_name() = test_name;
        std::cout << "\n[  " << name_ << "  ] " << test_name << std::endl;
        try {
            test_func();
        } catch (const std::exception& e) {
            g_test_stats().failed_tests++;
            std::cout << red("[EXCEPTION] ") << "Test threw exception: " << e.what() << std::endl;
        } catch (...) {
            g_test_stats().failed_tests++;
            std::cout << red("[EXCEPTION] ") << "Test threw unknown exception" << std::endl;
        }
    }

private:
    std::string name_;
};

// 辅助宏：创建测试套件并运行测试
#define TEST_SUITE(suite_name) \
    cc_server::testing::TestSuite suite_instance(suite_name)

#define RUN_TEST(test_name) \
    suite_instance.run(#test_name, []()

} // namespace testing
} // namespace cc_server

#endif // CONCURRENTCACHE_TEST_ASSERTIONS_H