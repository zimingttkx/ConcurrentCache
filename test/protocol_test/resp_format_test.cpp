// RespEncoder::format_double 的文本形态测试
//
// 分数是以**文本**送出客户端的，所以格式化规则就是数据契约的一部分。
// 这条测试钉两件事：
//   1. 整数值不能带小数点尾巴（2.0 必须写成 "2"，这是 Redis 的回复）；
//   2. 读回来必须还是同一个 double——"0.1234567" 打成 "0.123457" 是丢数值，
//      不只是不好看。以前 ZSCORE / ZRANGE WITHSCORES 用的 std::to_string(double)
//      等价于 %f，两条都不满足。

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../trace/test_assertions.h"
#include "protocol/resp.h"

namespace cc_server {
namespace testing {

namespace {

struct Sample {
    double value;
    const char* text;
};

// 逐个报出 Redis 在该输入下会给的字符串
const Sample kExactText[] = {
    {2.0, "2"},
    {0.0, "0"},
    {-0.0, "-0"},
    {1.0, "1"},
    {-3.0, "-3"},
    {1e20, "100000000000000000000"},
    {9007199254740992.0, "9007199254740992"},   // 2^53
    {0.1, "0.1"},
    {1.5, "1.5"},
    {0.5, "0.5"},
    {-1.25, "-1.25"},
    {0.1234567, "0.1234567"},                   // %f 会打成 0.123457
    {3.14159265358979, "3.14159265358979"},
    {1e-7, "0.0000001"},                        // %g 会打成 1e-07
    {0.3, "0.3"},
};

// 只要求"能原样读回来"的极端值：这些没有一个唯一正确的写法
const double kRoundTripOnly[] = {
    1.0 / 3.0,
    1e300,
    1.7976931348623157e308,          // DBL_MAX
    2.2250738585072014e-308,         // 最小正规数
    4.9406564584124654e-324,         // 最小次正规数
    1e15 + 0.5,
    1234567890123456.7,
    -1.2345e-15,
    0.1 + 0.2,                       // 0.30000000000000004
};

}  // namespace

void test_format_double_matches_redis_text_form() {
    TEST_SUITE("RESP format_double");

    for (const auto& sample : kExactText) {
        const std::string got = RespEncoder::format_double(sample.value);
        EXPECT_EQ(got, std::string(sample.text));
    }

    EXPECT_EQ(RespEncoder::format_double(std::nan("")), std::string("nan"));
    EXPECT_EQ(RespEncoder::format_double(INFINITY), std::string("inf"));
    EXPECT_EQ(RespEncoder::format_double(-INFINITY), std::string("-inf"));
}

void test_format_double_round_trips() {
    TEST_SUITE("RESP format_double");

    for (const double value : kRoundTripOnly) {
        const std::string text = RespEncoder::format_double(value);
        // 读回来的必须是同一个数。这一条就是"%f 截断"的真正判别。
        EXPECT_EQ(std::strtod(text.c_str(), nullptr), value);
        // 而且不许出现科学计数法——Redis 的回复里没有
        EXPECT_TRUE(text.find('e') == std::string::npos);
        EXPECT_TRUE(text.find('E') == std::string::npos);
        // 末尾也不许多出无意义的 0："1.50" 意味着最短表示没找到
        EXPECT_TRUE(text.find('.') == std::string::npos || text.back() != '0');
    }

    // 步进扫描：任何一段连续区间里的 double 都不允许读丢。
    // 这里把结果累计成一个数再断言，否则几万条 EXPECT 会把 CI 日志刷爆。
    int mismatches = 0;
    int exponents = 0;
    for (int i = 1; i <= 20000; ++i) {
        const double v = static_cast<double>(i) * 1.749872131749;
        const std::string tv = RespEncoder::format_double(v);
        if (std::strtod(tv.c_str(), nullptr) != v) ++mismatches;
        if (tv.find('e') != std::string::npos) ++exponents;

        const double w = -static_cast<double>(i) / 1024.0;
        const std::string tw = RespEncoder::format_double(w);
        if (std::strtod(tw.c_str(), nullptr) != w) ++mismatches;
        if (tw.find('e') != std::string::npos) ++exponents;
    }
    EXPECT_EQ(mismatches, 0);
    EXPECT_EQ(exponents, 0);
}

void run_all_resp_format_tests() {
    std::cout << "\n========================================\n";
    std::cout << "Running RESP double formatting Tests\n";
    std::cout << "========================================\n\n";

    test_format_double_matches_redis_text_form();
    test_format_double_round_trips();

    std::cout << "\n========================================\n";
    std::cout << "All RESP double formatting Tests Done!\n";
    std::cout << "========================================\n\n";
}

}  // namespace testing
}  // namespace cc_server
