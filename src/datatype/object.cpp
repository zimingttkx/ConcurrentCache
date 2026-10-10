#include "datatype/object.h"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cc_server {

CacheObject::CacheObject(const std::string& val)
    : type_(ObjectType::STRING), string_val_(val) {
    LOG_DEBUG(kModule, "CacheObject created as STRING, size=%zu", val.size());
}

CacheObject::CacheObject(std::vector<std::string> vals)
    : type_(ObjectType::LIST), list_val_(std::move(vals)) {
    LOG_DEBUG(kModule, "CacheObject created as LIST, size=%zu", list_val_.size());
}

size_t CacheObject::memory_size() const {
    switch (type_) {
        case ObjectType::STRING: return string_val_.size();
        case ObjectType::LIST: {
            size_t total = 0;
            for (const auto& s : list_val_) total += s.size();
            return total;
        }
        case ObjectType::HASH: {
            size_t total = 0;
            for (const auto& [k, v] : hash_val_) total += k.size() + v.size();
            return total;
        }
        case ObjectType::SET: {
            size_t total = 0;
            for (const auto& m : set_val_) total += m.size();
            return total;
        }
        case ObjectType::ZSET: {
            size_t total = 0;
            for (const auto& z : zset_val_) total += z.member.size() + sizeof(z.score);
            return total;
        }
        default: return 0;
    }
}

bool CacheObject::validate() const {
    if (type_ > ObjectType::ZSET) [[unlikely]] {
        LOG_ERROR(kModule, "validate - invalid type: %d", static_cast<int>(type_));
        return false;
    }
    LOG_TRACE(kModule, "validate - OK, type=%d", static_cast<int>(type_));
    return true;
}

void CacheObject::discard_string_payload() {
    // swap 而不是 clear()：clear() 不释放容量，一个大值 SET 完再 HSET
    // 会把整块缓冲区一直挂在已经变成 HASH 的对象上。
    std::string().swap(string_val_);
}

// List 操作
bool CacheObject::list_push(const std::string& val, bool front) {
    if (type_ != ObjectType::LIST && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "list_push - object is not LIST");
        return false;
    }
    if (type_ == ObjectType::STRING) {
        std::string old_val = std::move(string_val_);
        discard_string_payload();
        type_ = ObjectType::LIST;
        list_val_.clear();
        // 只有非空字符串才保留为列表首元素（避免默认构造的空字符串污染列表）
        if (!old_val.empty()) {
            list_val_.push_back(std::move(old_val));
        }
    }
    if (front) {
        list_val_.insert(list_val_.begin(), val);
    } else {
        list_val_.push_back(val);
    }
    LOG_DEBUG(kModule, "list_push - front=%d, new_size=%zu", front, list_val_.size());
    return true;
}

std::optional<std::string> CacheObject::list_pop(bool front) {
    if (type_ != ObjectType::LIST) [[unlikely]] {
        return std::nullopt;
    }
    if (list_val_.empty()) [[unlikely]] {
        return std::nullopt;
    }
    std::string result;
    if (front) {
        result = list_val_.front();
        list_val_.erase(list_val_.begin());
    } else {
        result = list_val_.back();
        list_val_.pop_back();
    }
    LOG_DEBUG(kModule, "list_pop - front=%d, remaining=%zu", front, list_val_.size());
    return result;
}

std::vector<std::string> CacheObject::list_range(long long start, long long stop) const {
    if (type_ != ObjectType::LIST) [[unlikely]] {
        return {};
    }

    size_t size = list_val_.size();
    if (size == 0) {
        return {};
    }

    // 处理负索引
    long long actual_start = start;
    long long actual_stop = stop;

    if (actual_start < 0) {
        actual_start = static_cast<long long>(size) + actual_start;
    }
    if (actual_stop < 0) {
        actual_stop = static_cast<long long>(size) + actual_stop;
    }

    // 边界检查
    if (actual_start < 0) actual_start = 0;
    if (actual_stop < 0) actual_stop = 0;
    if (static_cast<size_t>(actual_start) >= size) {
        return {};
    }
    if (static_cast<size_t>(actual_stop) >= size) {
        actual_stop = static_cast<long long>(size) - 1;
    }
    if (actual_start > actual_stop) {
        return {};
    }

    return std::vector<std::string>(list_val_.begin() + actual_start,
                                    list_val_.begin() + actual_stop + 1);
}

// Hash 操作
bool CacheObject::hash_set(const std::string& field, const std::string& value) {
    if (type_ != ObjectType::HASH && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "hash_set - object is not HASH");
        return false;
    }
    if (type_ == ObjectType::STRING) {
        discard_string_payload();
    }
    type_ = ObjectType::HASH;
    hash_val_[field] = value;
    LOG_DEBUG(kModule, "hash_set - field=%s, new_size=%zu", field.c_str(), hash_val_.size());
    return true;
}

std::optional<std::string> CacheObject::hash_get(const std::string& field) const {
    if (type_ != ObjectType::HASH) [[unlikely]] {
        return std::nullopt;
    }
    auto it = hash_val_.find(field);
    if (it == hash_val_.end()) [[likely]] {
        return std::nullopt;
    }
    return it->second;
}

bool CacheObject::hash_del(const std::string& field) {
    if (type_ != ObjectType::HASH) [[unlikely]] {
        return false;
    }
    auto erased = hash_val_.erase(field);
    LOG_DEBUG(kModule, "hash_del - field=%s, erased=%d", field.c_str(), erased);
    return erased > 0;
}

bool CacheObject::hash_exists(const std::string& field) const {
    if (type_ != ObjectType::HASH) [[unlikely]] {
        return false;
    }
    return hash_val_.contains(field);
}

std::vector<std::string> CacheObject::hash_fields() const {
    if (type_ != ObjectType::HASH) [[unlikely]] {
        return {};
    }
    std::vector<std::string> fields;
    fields.reserve(hash_val_.size());
    for (const auto& [k, v] : hash_val_) {
        fields.push_back(k);
    }
    return fields;
}

std::vector<std::pair<std::string, std::string>> CacheObject::hash_items() const {
    if (type_ != ObjectType::HASH) [[unlikely]] {
        return {};
    }
    return std::vector<std::pair<std::string, std::string>>(hash_val_.begin(), hash_val_.end());
}

// Set 操作
bool CacheObject::set_add(const std::string& member) {
    if (type_ != ObjectType::SET && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "set_add - object is not SET");
        return false;
    }
    if (type_ == ObjectType::STRING) {
        discard_string_payload();
    }
    type_ = ObjectType::SET;
    auto [it, inserted] = set_val_.insert(member);
    LOG_DEBUG(kModule, "set_add - member=%s, inserted=%d, new_size=%zu",
              member.c_str(), inserted, set_val_.size());
    return inserted;
}

bool CacheObject::set_remove(const std::string& member) {
    if (type_ != ObjectType::SET) [[unlikely]] {
        return false;
    }
    auto erased = set_val_.erase(member);
    LOG_DEBUG(kModule, "set_remove - member=%s, erased=%d, remaining=%zu",
              member.c_str(), erased, set_val_.size());
    return erased > 0;
}

bool CacheObject::set_contains(const std::string& member) const {
    if (type_ != ObjectType::SET) [[unlikely]] {
        return false;
    }
    return set_val_.contains(member);
}

std::vector<std::string> CacheObject::set_members() const {
    if (type_ != ObjectType::SET) [[unlikely]] {
        return {};
    }
    return std::vector<std::string>(set_val_.begin(), set_val_.end());
}

// ZSet 操作
bool CacheObject::zset_add(const std::string& member, double score) {
    if (type_ != ObjectType::ZSET && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "zset_add - object is not ZSET");
        return false;
    }
    if (type_ == ObjectType::STRING) {
        discard_string_payload();
    }
    type_ = ObjectType::ZSET;

    // 线性查找是否存在该 member（std::set 按 (score, member) 排序，无法直接按 member 查找）
    bool found = false;
    (void)found; // unused
    bool score_changed = false;
    auto it_to_erase = zset_val_.end();

    for (auto it = zset_val_.begin(); it != zset_val_.end(); ++it) {
        if (it->member == member) {
            found = true;
            if (it->score != score) {
                // 分数不同，需要更新
                score_changed = true;
                it_to_erase = it;
            }
            break;
        }
    }

    if (score_changed && it_to_erase != zset_val_.end()) {
        zset_val_.erase(it_to_erase);
    }

    // 插入新记录（如果 found 但 score_changed，会替换；其他情况按比较器去重）
    ZSetMember target{member, score};
    auto [new_it, inserted] = zset_val_.insert(target);
    LOG_DEBUG(kModule, "zset_add - member=%s, score=%f, inserted=%d, new_size=%zu",
              member.c_str(), score, inserted, zset_val_.size());
    // 返回 true 表示是新增（原来不存在）或分数有更新
    return inserted || score_changed;
}

bool CacheObject::zset_remove(const std::string& member) {
    if (type_ != ObjectType::ZSET) [[unlikely]] {
        return false;
    }
    // 线性查找（std::set 按 (score, member) 排序，无法直接按 member 查找）
    for (auto it = zset_val_.begin(); it != zset_val_.end(); ++it) {
        if (it->member == member) {
            zset_val_.erase(it);
            LOG_DEBUG(kModule, "zset_remove - member=%s, remaining=%zu", member.c_str(), zset_val_.size());
            return true;
        }
    }
    return false;
}

std::optional<double> CacheObject::zset_score(const std::string& member) const {
    if (type_ != ObjectType::ZSET) [[unlikely]] {
        return std::nullopt;
    }
    // 线性查找（std::set 按 (score, member) 排序，无法直接按 member 查找）
    for (const auto& zmember : zset_val_) {
        if (zmember.member == member) {
            return zmember.score;
        }
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, double>> CacheObject::zset_range_by_score(double min, double max, bool with_scores) const {
    if (type_ != ObjectType::ZSET) [[unlikely]] {
        return {};
    }
    std::vector<std::pair<std::string, double>> result;
    for (const auto& zmember : zset_val_) {
        if (zmember.score >= min && zmember.score <= max) {
            result.emplace_back(zmember.member, zmember.score);
        }
    }
    (void)with_scores;  // 保留参数兼容性
    return result;
}

std::vector<std::pair<std::string, double>> CacheObject::zset_range_by_index(long long start, long long stop, bool with_scores) const {
    if (type_ != ObjectType::ZSET) [[unlikely]] {
        return {};
    }

    size_t size = zset_val_.size();
    if (size == 0) {
        return {};
    }

    // 处理负索引（Redis 语义：-1 表示最后一个元素）
    long long actual_start = start;
    long long actual_stop = stop;

    if (actual_start < 0) {
        actual_start = static_cast<long long>(size) + actual_start;
    }
    if (actual_stop < 0) {
        actual_stop = static_cast<long long>(size) + actual_stop;
    }

    // 边界裁剪
    if (actual_start < 0) actual_start = 0;
    if (actual_stop < 0) return {};
    if (static_cast<size_t>(actual_start) >= size) return {};
    if (static_cast<size_t>(actual_stop) >= size) actual_stop = static_cast<long long>(size) - 1;
    if (actual_start > actual_stop) return {};

    std::vector<std::pair<std::string, double>> result;
    result.reserve(static_cast<size_t>(actual_stop - actual_start + 1));

    // std::set 按 score 排序，直接按位置迭代
    auto it = zset_val_.begin();
    std::advance(it, actual_start);
    for (long long i = actual_start; i <= actual_stop && it != zset_val_.end(); ++i, ++it) {
        result.emplace_back(it->member, it->score);
    }

    (void)with_scores;  // 保留参数兼容性
    return result;
}

std::vector<std::pair<std::string, double>> CacheObject::zset_all() const {
    if (type_ != ObjectType::ZSET) [[unlikely]] {
        return {};
    }
    std::vector<std::pair<std::string, double>> result;
    result.reserve(zset_val_.size());
    for (const auto& zmember : zset_val_) {
        result.emplace_back(zmember.member, zmember.score);
    }
    return result;
}

namespace {
    // 载荷框架：一行类型标签，之后是若干条"长度前缀记录" —— "<字节数>\n<原始字节>"。
    //
    // 早期版本每条记录以 "\n" 结尾，于是内容里带换行的元素会被提前收条：
    //     LPUSH k "a\nb"  →  "LIST\n1\na\nb\n"  →  解码器读出 1 个元素 "a"，"b" 丢掉
    // MIGRATE 和复制的键流都走这份载荷，所以那是静默的数据缺损，不是格式好不好看的问题。
    void append_record(std::string& out, const std::string& payload) {
        out += std::to_string(payload.size());
        out += '\n';
        out += payload;
    }

    bool read_record(const std::string& buf, size_t& pos, std::string& out) {
        const size_t nl = buf.find('\n', pos);
        if (nl == std::string::npos) return false;
        const std::string head = buf.substr(pos, nl - pos);
        unsigned long long len = 0;
        const char* first = head.data();
        const std::from_chars_result r = std::from_chars(first, first + head.size(), len);
        if (r.ec != std::errc() || r.ptr != first + head.size()) return false;   // 头部不是纯十进制
        const size_t available = buf.size() - (nl + 1);
        if (len > static_cast<unsigned long long>(available)) return false;       // 声明长度超过剩余字节
        out.assign(buf, nl + 1, static_cast<size_t>(len));
        pos = nl + 1 + static_cast<size_t>(len);
        return true;
    }

    bool read_count(const std::string& buf, size_t& pos, size_t& count) {
        std::string head;
        if (!read_record(buf, pos, head)) return false;
        unsigned long long v = 0;
        const char* first = head.data();
        const std::from_chars_result r = std::from_chars(first, first + head.size(), v);
        if (r.ec != std::errc() || r.ptr != first + head.size()) return false;
        count = static_cast<size_t>(v);
        return true;
    }

    bool read_score(const std::string& text, double& score) {
        const char* first = text.data();
        const std::from_chars_result r = std::from_chars(first, first + text.size(), score);
        if (r.ec != std::errc() || r.ptr != first + text.size()) return false;
        // 与 ZADD 的入参判断一致：NaN/Inf 不是合法的 sorted set 分数（旧载荷里也不该出现）
        return !std::isnan(score) && !std::isinf(score);
    }
}  // namespace

std::string CacheObject::serialize() const {
    std::string result;
    result.reserve(256);  // 预分配

    switch (type_) {
        case ObjectType::STRING: {
            result += "STRING\n";
            append_record(result, string_val_);
            break;
        }
        case ObjectType::LIST: {
            result += "LIST\n";
            append_record(result, std::to_string(list_val_.size()));
            for (const auto& elem : list_val_) {
                append_record(result, elem);
            }
            break;
        }
        case ObjectType::HASH: {
            result += "HASH\n";
            append_record(result, std::to_string(hash_val_.size()));
            for (const auto& [k, v] : hash_val_) {
                append_record(result, k);
                append_record(result, v);
            }
            break;
        }
        case ObjectType::SET: {
            result += "SET\n";
            append_record(result, std::to_string(set_val_.size()));
            for (const auto& m : set_val_) {
                append_record(result, m);
            }
            break;
        }
        case ObjectType::ZSET: {
            // 修复 P1-13：double 用 %.17g 保证与读侧精度往返一致
            // （否则 std::to_string 只 6 位小数，0.123456789 会丢精度）
            result += "ZSET\n";
            append_record(result, std::to_string(zset_val_.size()));
            char score_buf[32];
            for (const auto& z : zset_val_) {
                std::snprintf(score_buf, sizeof(score_buf), "%.17g", z.score);
                append_record(result, z.member);
                append_record(result, std::string(score_buf));
            }
            break;
        }
        default:
            // 未知类型给空载荷：读侧看到没有类型标签就判失败，不会当成空串键
            return {};
    }
    return result;
}

bool CacheObject::deserialize(const std::string& payload, std::string& err) {
    err.clear();

    const size_t nl = payload.find('\n');
    if (nl == std::string::npos) {
        err = "payload has no type tag";
        return false;
    }
    const std::string type = payload.substr(0, nl);
    size_t pos = nl + 1;

    // 记录读完必须正好落在末尾。多出来的尾巴说明发的人用的是别的框架
    // （比如早期那种"内容里带换行就提前收条"的写法），这时必须失败，
    // 不能"读到哪算哪" —— 旧实现遇到截断载荷会 break 出已读到的部分然后照样回 +OK。
    auto finish = [&](const char* what) -> bool {
        if (pos != payload.size()) {
            err = std::string(what) + ": trailing bytes in payload";
            return false;
        }
        return true;
    };

    if (type == "STRING") {
        std::string value;
        if (!read_record(payload, pos, value)) {
            err = "truncated STRING payload";
            return false;
        }
        set_string(value);
        return finish("STRING");
    }

    if (type == "LIST") {
        size_t count = 0;
        if (!read_count(payload, pos, count)) {
            err = "bad LIST element count";
            return false;
        }
        std::vector<std::string> elems;
        elems.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            std::string elem;
            if (!read_record(payload, pos, elem)) {
                err = "truncated LIST payload: wanted " + std::to_string(count) + " elements";
                return false;
            }
            elems.push_back(std::move(elem));
        }
        if (!finish("LIST")) return false;
        for (const auto& e : elems) {
            list_push(e);
        }
        return true;
    }

    if (type == "HASH") {
        size_t count = 0;
        if (!read_count(payload, pos, count)) {
            err = "bad HASH field count";
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string field, value;
            if (!read_record(payload, pos, field) || !read_record(payload, pos, value)) {
                err = "truncated HASH payload: wanted " + std::to_string(count) + " pairs";
                return false;
            }
            hash_set(field, value);
        }
        return finish("HASH");
    }

    if (type == "SET") {
        size_t count = 0;
        if (!read_count(payload, pos, count)) {
            err = "bad SET member count";
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string member;
            if (!read_record(payload, pos, member)) {
                err = "truncated SET payload: wanted " + std::to_string(count) + " members";
                return false;
            }
            set_add(member);
        }
        return finish("SET");
    }

    if (type == "ZSET") {
        size_t count = 0;
        if (!read_count(payload, pos, count)) {
            err = "bad ZSET member count";
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string member, score_text;
            if (!read_record(payload, pos, member) || !read_record(payload, pos, score_text)) {
                err = "truncated ZSET payload: wanted " + std::to_string(count) + " members";
                return false;
            }
            double score = 0;
            if (!read_score(score_text, score)) {
                err = "ZSET score is not a finite number: '" + score_text + "'";
                return false;
            }
            zset_add(member, score);
        }
        return finish("ZSET");
    }

    err = "unsupported payload type '" + type + "'";
    return false;
}

}  // namespace cc_server
