#include "datatype/object.h"

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

// List 操作
bool CacheObject::list_push(const std::string& val, bool front) {
    if (type_ != ObjectType::LIST && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "list_push - object is not LIST");
        return false;
    }
    if (type_ == ObjectType::STRING) {
        std::string old_val = std::move(string_val_);
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

bool CacheObject::list_set(size_t index, const std::string& val) {
    if (type_ != ObjectType::LIST) [[unlikely]] {
        LOG_WARN(kModule, "list_set - object is not LIST");
        return false;
    }
    if (index >= list_val_.size()) [[unlikely]] {
        LOG_WARN(kModule, "list_set - index %zu out of range %zu", index, list_val_.size());
        return false;
    }
    list_val_[index] = val;
    LOG_DEBUG(kModule, "list_set - index=%zu", index);
    return true;
}

std::optional<std::string> CacheObject::list_get(long long index) const {
    if (type_ != ObjectType::LIST) [[unlikely]] {
        return std::nullopt;
    }
    size_t size = list_val_.size();
    if (size == 0) {
        return std::nullopt;
    }
    long long actual_index = index;
    if (actual_index < 0) {
        actual_index = static_cast<long long>(size) + actual_index;
    }
    if (actual_index < 0 || static_cast<size_t>(actual_index) >= size) [[unlikely]] {
        LOG_WARN(kModule, "list_get - index %lld out of range %zu", actual_index, size);
        return std::nullopt;
    }
    return list_val_[static_cast<size_t>(actual_index)];
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

bool CacheObject::list_trim(long long start, long long stop) {
    if (type_ != ObjectType::LIST) [[unlikely]] {
        LOG_WARN(kModule, "list_trim - object is not LIST");
        return false;
    }

    size_t size = list_val_.size();
    if (size == 0) {
        return true;
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
    if (static_cast<size_t>(actual_start) >= size ||
        static_cast<size_t>(actual_stop) >= size ||
        actual_start > actual_stop) [[unlikely]] {
        LOG_WARN(kModule, "list_trim - invalid range: start=%lld, stop=%lld, size=%zu",
                 actual_start, actual_stop, size);
        return false;
    }

    list_val_.erase(list_val_.begin(), list_val_.begin() + actual_start);
    list_val_.erase(list_val_.begin() + (actual_stop - actual_start + 1), list_val_.end());
    LOG_DEBUG(kModule, "list_trim - new_size=%zu", list_val_.size());
    return true;
}

// Hash 操作
bool CacheObject::hash_set(const std::string& field, const std::string& value) {
    if (type_ != ObjectType::HASH && type_ != ObjectType::STRING) [[unlikely]] {
        LOG_WARN(kModule, "hash_set - object is not HASH");
        return false;
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

std::string CacheObject::serialize() const {
    std::string result;
    result.reserve(256);  // 预分配

    switch (type_) {
        case ObjectType::STRING: {
            // 格式：STRING\n<value>
            result += "STRING\n";
            result += string_val_;
            break;
        }
        case ObjectType::LIST: {
            // 格式：LIST\n<size>\n<element1>\n<element2>\n...
            result += "LIST\n";
            result += std::to_string(list_val_.size());
            result += "\n";
            for (const auto& elem : list_val_) {
                result += elem + "\n";
            }
            break;
        }
        case ObjectType::HASH: {
            // 格式：HASH\n<size>\n<field1>\n<value1>\n<field2>\n<value2>\n...
            result += "HASH\n";
            result += std::to_string(hash_val_.size());
            result += "\n";
            for (const auto& [k, v] : hash_val_) {
                result += k + "\n" + v + "\n";
            }
            break;
        }
        case ObjectType::SET: {
            // 格式：SET\n<size>\n<member1>\n<member2>\n...
            result += "SET\n";
            result += std::to_string(set_val_.size());
            result += "\n";
            for (const auto& m : set_val_) {
                result += m + "\n";
            }
            break;
        }
        case ObjectType::ZSET: {
            // 格式：ZSET\n<size>\n<member1>\n<score1>\n<member2>\n<score2>\n...
            result += "ZSET\n";
            result += std::to_string(zset_val_.size());
            result += "\n";
            for (const auto& z : zset_val_) {
                result += z.member + "\n" + std::to_string(z.score) + "\n";
            }
            break;
        }
        default:
            result = "";
            break;
    }
    return result;
}

}  // namespace cc_server
