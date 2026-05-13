//
// Created by Administrator on 2026/5/13.
//

#include "restore_cmd.h"
#include "protocol/resp.h"
#include "base/log.h"
#include <sstream>

namespace cc_server {

std::string RestoreCommand::execute(const std::vector<std::string>& args) {
    // RESTORE key ttl_ms serialized_value [REPLACE]
    if (args.size() < 4) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'restore' command");
    }

    const std::string& key = args[1];
    const std::string& ttl_str = args[2];
    const std::string& serialized = args[3];

    // 解析 TTL
    int64_t ttl_ms = -1;
    try {
        ttl_ms = std::stoll(ttl_str);
    } catch (...) {
        return RespEncoder::encode_error("ERR invalid TTL");
    }

    // 检查是否已有该键
    bool replace = false;
    if (args.size() > 4 && args[4] == "REPLACE") {
        replace = true;
    }

    if (!replace && GlobalStorage::instance().exist(key)) {
        return RespEncoder::encode_error("BUSYKEY Target key name already exists");
    }

    // 反序列化数据
    std::istringstream ss(serialized);
    std::string type;
    if (!std::getline(ss, type)) {
        return RespEncoder::encode_error("ERR invalid serialized data");
    }

    CacheObject obj;

    if (type == "STRING" || type.empty()) {
        // STRING 类型：STRING\n<value>
        std::string value = serialized.substr(type.size());
        // 跳过类型后面的换行符
        if (!value.empty() && value[0] == '\n') {
            value = value.substr(1);
        }
        obj.set_string(value);
    } else if (type == "LIST") {
        // LIST 类型：LIST\n<size>\n<elem1>\n<elem2>\n...
        std::string size_line;
        if (!std::getline(ss, size_line)) {
            return RespEncoder::encode_error("ERR invalid serialized data for LIST");
        }
        size_t size = std::stoull(size_line);
        std::vector<std::string> elems;
        for (size_t i = 0; i < size; i++) {
            std::string elem;
            if (!std::getline(ss, elem)) {
                break;
            }
            elems.push_back(elem);
        }
        for (const auto& e : elems) {
            obj.list_push(e);
        }
    } else if (type == "HASH") {
        // HASH 类型：HASH\n<size>\n<field1>\n<value1>\n...
        std::string size_line;
        if (!std::getline(ss, size_line)) {
            return RespEncoder::encode_error("ERR invalid serialized data for HASH");
        }
        size_t size = std::stoull(size_line);
        for (size_t i = 0; i < size; i++) {
            std::string field, value;
            if (!std::getline(ss, field) || !std::getline(ss, value)) {
                break;
            }
            obj.hash_set(field, value);
        }
    } else if (type == "SET") {
        // SET 类型：SET\n<size>\n<member1>\n<member2>\n...
        std::string size_line;
        if (!std::getline(ss, size_line)) {
            return RespEncoder::encode_error("ERR invalid serialized data for SET");
        }
        size_t size = std::stoull(size_line);
        for (size_t i = 0; i < size; i++) {
            std::string member;
            if (!std::getline(ss, member)) {
                break;
            }
            obj.set_add(member);
        }
    } else if (type == "ZSET") {
        // ZSET 类型：ZSET\n<size>\n<member1>\n<score1>\n...
        std::string size_line;
        if (!std::getline(ss, size_line)) {
            return RespEncoder::encode_error("ERR invalid serialized data for ZSET");
        }
        size_t size = std::stoull(size_line);
        for (size_t i = 0; i < size; i++) {
            std::string member, score_str;
            if (!std::getline(ss, member) || !std::getline(ss, score_str)) {
                break;
            }
            double score = std::stod(score_str);
            obj.zset_add(member, score);
        }
    } else {
        return RespEncoder::encode_error("ERR unsupported serialized data type");
    }

    // 存储对象
    if (ttl_ms > 0) {
        GlobalStorage::instance().set_with_expire(key, obj, ttl_ms);
    } else {
        GlobalStorage::instance().set(key, obj);
    }

    LOG_INFO(RESTORE, "RESTORE completed: key=%s, ttl_ms=%ld", key.c_str(), ttl_ms);
    return RespEncoder::encode_simple_string("OK");
}

} // namespace cc_server
