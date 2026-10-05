//
// Created by Administrator on 2026/5/13.
//

#include "restore_cmd.h"
#include "protocol/resp.h"
#include "base/log.h"
#include <string>

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

    // 反序列化交给 CacheObject 自己做：载荷的编解码是它的事，放在这里就会有两份
    // 互相对不上的框架描述（旧代码正是如此 —— 遇到截断载荷 break 出已读到的部分，
    // 然后照样回 +OK，副本/目标节点拿到一个"成功但少了几条"的键）。
    CacheObject obj;
    std::string decode_err;
    if (!obj.deserialize(serialized, decode_err)) {
        LOG_ERROR(RESTORE, "RESTORE - malformed payload for key=%s: %s", key.c_str(), decode_err.c_str());
        return RespEncoder::encode_error("ERR Invalid or malformed serialized payload");
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
