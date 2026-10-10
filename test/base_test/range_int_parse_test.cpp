// base/int_parse.h 与 base/range_clip.h 的纯逻辑单元测试。
//
// 两个头文件都是 header-only、不碰 logger/线程/POSIX，所以这组断言在任何平台上都能直接编译，
// 也能用 -DRANGE_INT_PARSE_STANDALONE 单独跑。CI 里它挂在 concurrentcache-v3-tests 上
// （gate 标签，required check asan-smoke 会跑）。
//
// 钉住的是"静默收下拼错的参数"这一类缺陷：INCRBY k 10abc 加 10、INCR 把值 "12abc" 当成 12、
// 同一个 `k 0 -3` 在 LRANGE 与 ZRANGE 上给出不同答案 —— 三条都在此断言。
#include "base/int_parse.h"
#include "base/range_clip.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void expect_ll(const char* what, const std::string& in, bool want_ok, long long want = 0) {
    long long out = 0;
    const bool ok = cc_server::base::parse_ll_strict(in, out);
    if (ok != want_ok || (want_ok && out != want)) {
        ++failures;
        std::printf("FAIL %s: parse(\"%s\") -> ok=%d val=%lld, want ok=%d val=%lld\n",
                    what, in.c_str(), static_cast<int>(ok), out,
                    static_cast<int>(want_ok), want);
        return;
    }
    std::printf("PASS %s\n", what);
}

void expect_clip(const char* what, std::size_t size, long long start, long long stop,
                 bool want_nonempty, long long want_start = 0, long long want_stop = 0) {
    long long a = -1;
    long long b = -1;
    const bool nonempty = cc_server::base::clip_range(size, start, stop, a, b);
    if (nonempty != want_nonempty || (want_nonempty && (a != want_start || b != want_stop))) {
        ++failures;
        std::printf("FAIL %s: clip(size=%zu, %lld, %lld) -> nonempty=%d [%lld..%lld], "
                    "want nonempty=%d [%lld..%lld]\n",
                    what, size, start, stop, static_cast<int>(nonempty), a, b,
                    static_cast<int>(want_nonempty), want_start, want_stop);
        return;
    }
    std::printf("PASS %s\n", what);
}

void run_cases() {
    // 严格整数：前缀读得出来但没读完的，一律拒
    expect_ll("plain", "10", true, 10);
    expect_ll("negative", "-10", true, -10);
    expect_ll("rejects trailing junk", "10abc", false);
    expect_ll("rejects decimal point", "1.5", false);
    expect_ll("rejects leading space", " 10", false);
    expect_ll("rejects plus sign", "+10", false);
    expect_ll("rejects empty", "", false);
    expect_ll("accepts int64 max", "9223372036854775807", true, 9223372036854775807LL);
    expect_ll("rejects int64 overflow", "9223372036854775808", false);

    // 索引裁剪（2 个元素的表）
    expect_clip("full range", 2, 0, -1, true, 0, 1);
    expect_clip("only last", 2, -1, -1, true, 1, 1);
    expect_clip("stop lands before start", 2, 1, -2, false);
    expect_clip("zero-length table", 0, 0, -1, false);
    // LRANGE 以前把负 stop 夹到 0，于是 `LRANGE k 0 -3` 返回下标 0 的元素；
    // ZRANGE 与 Redis 都给空。这条断言就是那个分叉点。
    expect_clip("stop past the front is empty", 2, 0, -3, false);
    expect_clip("start past the end is empty", 2, 5, 10, false);
    expect_clip("stop clamped to last", 2, 0, 99, true, 0, 1);
    expect_clip("start clamped to zero", 2, -99, 99, true, 0, 1);
    expect_clip("reversed is empty", 2, 1, 0, false);
}

}  // namespace

namespace cc_server {
namespace testing {

void run_all_range_int_parse_tests() {
    std::printf("\n========================================\n");
    std::printf("Running Strict Integer / Range Clip Tests\n");
    std::printf("========================================\n\n");
    run_cases();
    if (failures != 0) {
        std::printf("\n严格整数与索引裁剪检查失败 %d 项\n", failures);
        std::fflush(stdout);
        std::exit(1);
    }
}

}  // namespace testing
}  // namespace cc_server

#ifdef RANGE_INT_PARSE_STANDALONE
int main() {
    run_cases();
    std::printf("%s range_int_parse: %d 项失败\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
#endif
