#ifndef CONCURRENTCACHE_COMMAND_STRING_CMD_H
#define CONCURRENTCACHE_COMMAND_STRING_CMD_H

#include "command.h"
#include "cache/storage.h"
#include "protocol/resp.h"

namespace cc_server {

    /**
     * @brief GetCommand - GET 命令实现
     *
     * GET 命令：获取指定 key 的值
     * 语法：GET key
     * 返回：
     *   - 如果 key 存在：返回其值（Bulk String 格式）
     *   - 如果 key 不存在：返回 nil
     *
     * RESP 格式：
     *   GET key      → $5\r\nhello\r\n  或  $-1\r\n（不存在）
     */
    class GetCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            // 参数验证：GET 命令需要 2 个参数 [命令名, key]
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'get' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            // key 不存在时返回 nil
            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            return RespEncoder::encode_bulk_string(result.value());
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<GetCommand>(*this);
        }
    };

    /**
     * @brief SetCommand - SET 命令实现
     *
     * SET 命令：设置指定 key 的值
     * 语法：SET key value
     * 返回：
     *   - 成功：OK
     *
     * RESP 格式：
     *   SET key value → +OK\r\n
     */
    class SetCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            // 参数验证：SET 命令需要 3 个参数 [命令名, key, value]
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'set' command");
            }

            const std::string& key = args[1];
            const std::string& value = args[2];
            GlobalStorage::instance().set(key, value);

            return RespEncoder::encode_ok();
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SetCommand>(*this);
        }
    };

    /**
     * @brief DelCommand - DEL 命令实现
     *
     * DEL 命令：删除指定 key
     * 语法：DEL key
     * 返回：
     *   - 删除成功：1
     *   - key 不存在：0
     *
     * RESP 格式：
     *   DEL key → :1\r\n 或 :0\r\n
     */
    class DelCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            // 参数验证：DEL 命令需要 2 个参数 [命令名, key]
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'del' command");
            }

            const std::string& key = args[1];
            bool deleted = GlobalStorage::instance().del(key);

            return RespEncoder::encode_integer(deleted ? 1 : 0);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DelCommand>(*this);
        }
    };

    /**
     * @brief ExistsCommand - EXISTS 命令实现
     *
     * EXISTS 命令：检查 key 是否存在
     * 语法：EXISTS key
     * 返回：
     *   - key 存在：1
     *   - key 不存在：0
     *
     * RESP 格式：
     *   EXISTS key → :1\r\n 或 :0\r\n
     */
    class ExistsCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            // 参数验证：EXISTS 命令需要 2 个参数 [命令名, key]
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'exists' command");
            }

            const std::string& key = args[1];
            bool exists = GlobalStorage::instance().exist(key);

            return RespEncoder::encode_integer(exists ? 1 : 0);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ExistsCommand>(*this);
        }
    };
}

#endif
