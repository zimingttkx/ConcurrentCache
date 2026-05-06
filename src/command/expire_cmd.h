#ifndef CONCURRENTCACHE_COMMAND_EXPIRE_CMD_H
#define CONCURRENTCACHE_COMMAND_EXPIRE_CMD_H

#include "command.h"
#include "cache/storage.h"
#include "protocol/resp.h"
#include "base/log.h"
#include <cassert>

namespace cc_server {

/**
 * @brief ExpireCommand - EXPIRE 命令
 *
 * 语法：EXPIRE key seconds
 * 返回：1=成功设置，0=键不存在
 *
 * RESP 格式：
 *   EXPIRE key 60 → :1\r\n 或 :0\r\n
 */
class ExpireCommand : public Command {
public:
    std::string execute(const std::vector<std::string>& args) override {
        // 参数验证：EXPIRE key seconds
        if (args.size() != 3) {
            LOG_WARN(EXPIRE, "EXPIRE - wrong argument count: %zu, expected 3", args.size());
            return RespEncoder::encode_error("ERR wrong number of arguments for 'expire' command");
        }

        const std::string& key = args[1];
        const std::string& seconds_str = args[2];

        // 参数防御性检查
        assert(!key.empty() && "EXPIRE command - key is empty");

        // 解析过期秒数
        try {
            int64_t seconds = std::stoll(seconds_str);
            if (seconds <= 0) {
                LOG_WARN(EXPIRE, "EXPIRE - invalid seconds <= 0: %ld for key=%s", seconds, key.c_str());
                return RespEncoder::encode_integer(0);
            }

            // 检查键是否存在
            auto& storage = GlobalStorage::instance();
            if (!storage.exist(key)) {
                LOG_DEBUG(EXPIRE, "EXPIRE - key not found: %s", key.c_str());
                return RespEncoder::encode_integer(0);
            }

            // 设置过期时间（转换为毫秒）
            storage.expire_dict().set(key, seconds * 1000);
            LOG_INFO(EXPIRE, "EXPIRE - key=%s, seconds=%ld, ttl_ms=%ld",
                    key.c_str(), seconds, seconds * 1000);
            return RespEncoder::encode_integer(1);

        } catch (const std::exception& e) {
            LOG_ERROR(EXPIRE, "EXPIRE - parse error for key=%s: %s", key.c_str(), e.what());
            return RespEncoder::encode_error("ERR value is not an integer");
        }
    }

    [[nodiscard]] std::unique_ptr<Command> clone() const override {
        return std::make_unique<ExpireCommand>(*this);
    }
};

/**
 * @brief TtlCommand - TTL 命令
 *
 * 语法：TTL key
 * 返回：剩余生存时间（秒），-2=不存在，-1=永不过期
 *
 * RESP 格式：
 *   TTL key → :60\r\n 或 :-2\r\n 或 :-1\r\n
 */
class TtlCommand : public Command {
public:
    std::string execute(const std::vector<std::string>& args) override {
        // 参数验证：TTL key
        if (args.size() != 2) {
            LOG_WARN(EXPIRE, "TTL - wrong argument count: %zu, expected 2", args.size());
            return RespEncoder::encode_error("ERR wrong number of arguments for 'ttl' command");
        }

        const std::string& key = args[1];
        assert(!key.empty() && "TTL command - key is empty");

        auto& storage = GlobalStorage::instance();

        // 检查键是否存在
        if (!storage.exist(key)) {
            // 如果存储中不存在，检查过期字典
            if (storage.expire_dict().contains(key)) {
                // 存在于过期字典但已过期（理论上不应该发生）
                LOG_WARN(EXPIRE, "TTL - inconsistency: key in expire_dict but not in storage: %s", key.c_str());
                return RespEncoder::encode_integer(-2);
            }
            LOG_DEBUG(EXPIRE, "TTL - key not found: %s", key.c_str());
            return RespEncoder::encode_integer(-2);
        }

        // 键存在于存储，检查是否有过期设置
        if (!storage.expire_dict().contains(key)) {
            // 存在于存储但无过期时间（永不过期）
            LOG_DEBUG(EXPIRE, "TTL - key has no expiration (never expire): %s", key.c_str());
            return RespEncoder::encode_integer(-1);
        }

        // 键存在且有过期设置，获取剩余 TTL
        int64_t ttl_ms = storage.expire_dict().get_ttl(key);
        if (ttl_ms <= 0) {
            // 已过期（理论上不应该发生，因为存储中该键应该被惰性删除了）
            LOG_WARN(EXPIRE, "TTL - key expired but still in storage: %s", key.c_str());
            return RespEncoder::encode_integer(-2);
        }

        int64_t ttl_sec = ttl_ms / 1000;
        LOG_DEBUG(EXPIRE, "TTL - key=%s, ttl_sec=%ld, ttl_ms=%ld", key.c_str(), ttl_sec, ttl_ms);
        return RespEncoder::encode_integer(ttl_sec);
    }

    [[nodiscard]] std::unique_ptr<Command> clone() const override {
        return std::make_unique<TtlCommand>(*this);
    }
};

/**
 * @brief PersistCommand - PERSIST 命令
 *
 * 语法：PERSIST key
 * 返回：1=成功移除过期，0=键不存在或无过期时间
 *
 * RESP 格式：
 *   PERSIST key → :1\r\n 或 :0\r\n
 */
class PersistCommand : public Command {
public:
    std::string execute(const std::vector<std::string>& args) override {
        // 参数验证：PERSIST key
        if (args.size() != 2) {
            LOG_WARN(EXPIRE, "PERSIST - wrong argument count: %zu, expected 2", args.size());
            return RespEncoder::encode_error("ERR wrong number of arguments for 'persist' command");
        }

        const std::string& key = args[1];
        assert(!key.empty() && "PERSIST command - key is empty");

        // 键必须存在
        auto& storage = GlobalStorage::instance();
        if (!storage.exist(key)) {
            LOG_DEBUG(EXPIRE, "PERSIST - key not found: %s", key.c_str());
            return RespEncoder::encode_integer(0);
        }

        // 移除过期时间
        bool success = storage.expire_dict().persist(key);
        LOG_INFO(EXPIRE, "PERSIST - key=%s, success=%d", key.c_str(), success);
        return RespEncoder::encode_integer(success ? 1 : 0);
    }

    [[nodiscard]] std::unique_ptr<Command> clone() const override {
        return std::make_unique<PersistCommand>(*this);
    }
};

/**
 * @brief SetexCommand - SETEX 命令
 *
 * 语法：SETEX key seconds value
 * 返回：OK
 *
 * RESP 格式：
 *   SETEX key 60 value → +OK\r\n
 */
class SetexCommand : public Command {
public:
    std::string execute(const std::vector<std::string>& args) override {
        // 参数验证：SETEX key seconds value
        if (args.size() != 4) {
            LOG_WARN(EXPIRE, "SETEX - wrong argument count: %zu, expected 4", args.size());
            return RespEncoder::encode_error("ERR wrong number of arguments for 'setex' command");
        }

        const std::string& key = args[1];
        const std::string& value = args[3];
        const std::string& seconds_str = args[2];

        // 参数防御性检查
        assert(!key.empty() && "SETEX command - key is empty");
        assert(!seconds_str.empty() && "SETEX command - seconds is empty");

        // 解析过期秒数
        try {
            int64_t seconds = std::stoll(seconds_str);
            if (seconds <= 0) {
                LOG_WARN(EXPIRE, "SETEX - invalid seconds <= 0: %ld for key=%s", seconds, key.c_str());
                return RespEncoder::encode_error("ERR invalid expire time");
            }

            auto& storage = GlobalStorage::instance();

            // 先设置值
            storage.set(key, value);

            // 再设置过期时间
            storage.expire_dict().set(key, seconds * 1000);

            LOG_INFO(EXPIRE, "SETEX - key=%s, seconds=%ld, value_len=%zu, ttl_ms=%ld",
                    key.c_str(), seconds, value.size(), seconds * 1000);
            return RespEncoder::encode_ok();

        } catch (const std::exception& e) {
            LOG_ERROR(EXPIRE, "SETEX - parse error for key=%s: %s", key.c_str(), e.what());
            return RespEncoder::encode_error("ERR value is not an integer");
        }
    }

    [[nodiscard]] std::unique_ptr<Command> clone() const override {
        return std::make_unique<SetexCommand>(*this);
    }
};

}  // namespace cc_server

#endif
