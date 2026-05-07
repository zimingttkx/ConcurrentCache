//
// Created by Administrator on 2026/5/7.
//

#ifndef OBJECT_H
#define OBJECT_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <variant>
#include <cstdint>

#include "base/log.h"

namespace cc_server {
    // 对象类型
    enum class ObjectType : uint8_t {
        STRING = 0,
        LIST = 1,
        HASH = 2,
        SET = 3,
        ZSET = 4
    };

    // 统一对象封装 使用STL实现的数据类型
    class CacheObject {
    public:
        CacheObject() : type_(ObjectType::STRING){}

        explicit CacheObject(const std::string& val);
        explicit CacheObject(std::vector<std::string> vals);

        ObjectType type() const { return type_;}

        size_t memory_size() const;

        bool validate() const;

        // string 操作
        std::optional<std::string> get_string() const {
            return string_val_;
        }
        void set_string(const std::string& val) {
            type_ = ObjectType::STRING;
            string_val_ = val;
            LOG_DEBUG(kModule, "Set_string - size = %zu", val.size());
        }

        // List操作
        bool list_push(const std::string& val, bool front = false);

        std::optional<std::string> list_pop(bool front = false);

        bool list_set(size_t index, const std::string& val);

        std::optional<std::string> list_get(long long index) const;

        std::vector<std::string> list_range(long long start, long long stop) const;

        // 保留列表指定范围内的元素，删除范围外的所有元素
        bool list_trim(long long start, long long stop);

        size_t list_size() const{ return list_val_.size();}

        // Hash操作
        bool hash_set(const std::string& field, const std::string& value);

        std::optional<std::string> hash_get(const std::string& field) const;

        bool hash_del(const std::string& field);

        bool hash_exists(const std::string& field) const;

        size_t hash_size() const { return hash_val_.size();}

        std::vector<std::string> hash_fields() const;

        std::vector<std::pair<std::string, std::string>> hash_items() const;

        // Set操作
        bool set_add(const std::string& member);

        bool set_remove(const std::string& member);

        bool set_contains(const std::string& member) const;

        size_t set_size() const { return set_val_.size();}

        std::vector<std::string> set_members() const;

        // ZSet操作
        bool zset_add(const std::string& member, double score);

        bool zset_remove(const std::string& member);

        std::optional<double> zset_score(const std::string& member) const;

        size_t zset_size() const { return zset_val_.size();}

        std::vector<std::pair<std::string, double>> zset_range_by_score(double min, double max, bool with_scores = false) const;

    private:
        // ZSet 成员结构体（按分数排序）
        struct ZSetMember {
            std::string member;
            double score;
            bool operator<(const ZSetMember& other) const {
                if (score < other.score) return true;
                if (score > other.score) return false;
                return member < other.member;  // 分数相同时按字典序
            }
        };

        ObjectType type_;

        std::string string_val_;
        std::vector<std::string> list_val_;
        std::unordered_map<std::string, std::string> hash_val_;
        std::unordered_set<std::string> set_val_;
        std::set<ZSetMember> zset_val_;  // 有序集合

        static constexpr const char* kModule = "OBJECT";
    };

}


#endif //OBJECT_H
