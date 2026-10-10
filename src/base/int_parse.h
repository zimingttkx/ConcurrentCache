#pragma once
// 严格整数解析：整个字符串都被吃掉才算一个合法的数。
//
// 为什么不能用 std::stoll：它读完前缀就返回，"10abc" 给出 10、"1.5" 给出 1，
// 于是 `INCRBY k 10abc` 会静默加 10、`INCR` 会把值是 "12abc" 的计数器当成 12。
// 计数器是要被别处读的，静默收下拼错的参数比报错危险得多。
//
// 与 Redis 的 string2l（strtoll 口径）相比，本实现不接受前导空白与 '+' 号：
// " 5" 与 "+5" 在这里会被拒。这是刻意收紧，不是漏掉，api.md 里按这个口径写。
#include <charconv>
#include <string>

namespace cc_server {
namespace base {

inline bool parse_ll_strict(const std::string& s, long long& out) {
    if (s.empty()) {
        return false;
    }
    const char* first = s.data();
    const char* last = first + s.size();
    const std::from_chars_result r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last;
}

}  // namespace base
}  // namespace cc_server
