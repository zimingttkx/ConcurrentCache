#ifndef CONCURRENTCACHE_COMMAND_STRING_CMD_H
#define CONCURRENTCACHE_COMMAND_STRING_CMD_H

#include "command.h"
#include "cache/storage.h"
#include "protocol/resp.h"
#include "datatype/object.h"
#include <chrono>
#include <random>

#include "persistence/rdb.h"

namespace cc_server {

    /**
     * @brief PingCommand - PING 命令实现
     *
     * PING 命令：返回 PONG 或指定消息
     * 语法：PING [message]
     * 返回：
     *   - 无参数：PONG
     *   - 有参数：返回指定消息
     *
     * RESP 格式：
     *   PING        → +PONG\r\n
     *   PING hello  → $5\r\nhello\r\n
     */
    class PingCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() == 1) {
                return RespEncoder::encode_simple_string("PONG");
            }
            return RespEncoder::encode_bulk_string(args[1]);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<PingCommand>();
        }
    };

    /**
     * @brief GetCommand - GET 命令实现
     *
     * GET 命令：获取指定 key 的值
     * 语法：GET key
     * 返回：
     *   - 如果 key 存在：返回其值（Bulk String 格式）
     *   - 如果 key 不存在：返回 nil
     *   - 如果 key 不是字符串类型：返回 error
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

            // 从 CacheObject 提取字符串
            auto str = result.value().get_string();
            if (!str) {
                return RespEncoder::encode_error("ERR not a string");
            }

            return RespEncoder::encode_bulk_string(str.value());
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

            // 将字符串包装为 CacheObject
            GlobalStorage::instance().set(key, CacheObject(value));

            return RespEncoder::encode_ok();
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SetCommand>(*this);
        }
    };

    /**
     * @brief IncrCommand - INCR 命令实现
     *
     * INCR 命令：对指定 key 的值进行原子加 1
     * 语法：INCR key
     * 返回：递增后的整数值
     */
    class IncrCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'incr' command");
            }

            const std::string& key = args[1];
            auto& storage = GlobalStorage::instance();
            auto opt = storage.get(key);

            int64_t val = 0;
            if (opt.has_value()) {
                auto str_val = opt.value().get_string();
                if (!str_val.has_value()) {
                    return RespEncoder::encode_error("ERR value is not an integer");
                }
                try {
                    val = std::stoll(str_val.value());
                } catch (...) {
                    return RespEncoder::encode_error("ERR value is not an integer");
                }
            }

            val++;
            storage.set(key, CacheObject(std::to_string(val)));
            return RespEncoder::encode_integer(val);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<IncrCommand>(*this);
        }
    };

    /**
     * @brief DecrCommand - DECR 命令实现
     *
     * DECR 命令：对指定 key 的值进行原子减 1
     * 语法：DECR key
     * 返回：递减后的整数值
     */
    class DecrCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'decr' command");
            }

            const std::string& key = args[1];
            auto& storage = GlobalStorage::instance();
            auto opt = storage.get(key);

            int64_t val = 0;
            if (opt.has_value()) {
                auto str_val = opt.value().get_string();
                if (!str_val.has_value()) {
                    return RespEncoder::encode_error("ERR value is not an integer");
                }
                try {
                    val = std::stoll(str_val.value());
                } catch (...) {
                    return RespEncoder::encode_error("ERR value is not an integer");
                }
            }

            val--;
            storage.set(key, CacheObject(std::to_string(val)));
            return RespEncoder::encode_integer(val);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DecrCommand>(*this);
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

    // ==================== List 命令 ====================

    /**
     * @brief LpushCommand - LPUSH 命令实现
     *
     * LPUSH 命令：从列表左侧推入元素
     * 语法：LPUSH key value [value ...]
     * 返回：列表长度
     */
    class LpushCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() < 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'lpush' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            CacheObject obj;
            if (result.has_value()) {
                obj = std::move(result.value());
            }

            for (size_t i = 2; i < args.size(); ++i) {
                obj.list_push(args[i], true);
            }

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(static_cast<int64_t>(obj.list_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<LpushCommand>(*this);
        }
    };

    /**
     * @brief RpushCommand - RPUSH 命令实现
     *
     * RPUSH 命令：从列表右侧推入元素
     * 语法：RPUSH key value [value ...]
     * 返回：列表长度
     */
    class RpushCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() < 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'rpush' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            CacheObject obj;
            if (result.has_value()) {
                obj = std::move(result.value());
            }

            for (size_t i = 2; i < args.size(); ++i) {
                obj.list_push(args[i], false);
            }

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(static_cast<int64_t>(obj.list_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<RpushCommand>(*this);
        }
    };

    /**
     * @brief LpopCommand - LPOP 命令实现
     *
     * LPOP 命令：从列表左侧弹出元素
     * 语法：LPOP key
     * 返回：弹出的元素或 nil
     */
    class LpopCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'lpop' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            auto obj = std::move(result.value());
            auto val = obj.list_pop(true);

            if (val) {
                GlobalStorage::instance().set(key, obj);
                return RespEncoder::encode_bulk_string(val.value());
            }

            return RespEncoder::encode_nil();
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<LpopCommand>(*this);
        }
    };

    /**
     * @brief RpopCommand - RPOP 命令实现
     *
     * RPOP 命令：从列表右侧弹出元素
     * 语法：RPOP key
     * 返回：弹出的元素或 nil
     */
    class RpopCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'rpop' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            auto obj = std::move(result.value());
            auto val = obj.list_pop(false);

            if (val) {
                GlobalStorage::instance().set(key, obj);
                return RespEncoder::encode_bulk_string(val.value());
            }

            return RespEncoder::encode_nil();
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<RpopCommand>(*this);
        }
    };

    /**
     * @brief LlenCommand - LLEN 命令实现
     *
     * LLEN 命令：获取列表长度
     * 语法：LLEN key
     * 返回：列表长度
     */
    class LlenCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'llen' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            return RespEncoder::encode_integer(static_cast<int64_t>(result.value().list_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<LlenCommand>(*this);
        }
    };

    /**
     * @brief LrangeCommand - LRANGE 命令实现
     *
     * LRANGE 命令：获取列表范围内的元素
     * 语法：LRANGE key start stop
     * 返回：元素列表
     */
    class LrangeCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 4) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'lrange' command");
            }

            const std::string& key = args[1];

            long long start, stop;
            try {
                start = std::stoll(args[2]);
                stop = std::stoll(args[3]);
            } catch (...) {
                return RespEncoder::encode_error("ERR invalid integer");
            }

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_array({});
            }

            auto values = result.value().list_range(start, stop);
            return RespEncoder::encode_array(values);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<LrangeCommand>(*this);
        }
    };

    // ==================== Hash 命令 ====================

    /**
     * @brief HsetCommand - HSET 命令实现
     *
     * HSET 命令：设置哈希字段
     * 语法：HSET key field value
     * 返回：1 新增，0 更新
     */
    class HsetCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 4) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'hset' command");
            }

            const std::string& key = args[1];
            const std::string& field = args[2];
            const std::string& value = args[3];

            auto result = GlobalStorage::instance().get(key);

            CacheObject obj;
            if (result.has_value()) {
                obj = std::move(result.value());
            }

            bool is_new = !obj.hash_exists(field);
            obj.hash_set(field, value);

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(is_new ? 1 : 0);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<HsetCommand>(*this);
        }
    };

    /**
     * @brief HgetCommand - HGET 命令实现
     *
     * HGET 命令：获取哈希字段值
     * 语法：HGET key field
     * 返回：值或 nil
     */
    class HgetCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'hget' command");
            }

            const std::string& key = args[1];
            const std::string& field = args[2];

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            auto val = result.value().hash_get(field);
            if (!val) {
                return RespEncoder::encode_nil();
            }

            return RespEncoder::encode_bulk_string(val.value());
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<HgetCommand>(*this);
        }
    };

    /**
     * @brief HdelCommand - HDEL 命令实现
     *
     * HDEL 命令：删除哈希字段
     * 语法：HDEL key field [field ...]
     * 返回：删除的字段数量
     */
    class HdelCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() < 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'hdel' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            auto obj = std::move(result.value());
            size_t deleted = 0;

            for (size_t i = 2; i < args.size(); ++i) {
                if (obj.hash_del(args[i])) {
                    deleted++;
                }
            }

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(static_cast<int64_t>(deleted));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<HdelCommand>(*this);
        }
    };

    /**
     * @brief HlenCommand - HLEN 命令实现
     *
     * HLEN 命令：获取哈希字段数量
     * 语法：HLEN key
     * 返回：字段数量
     */
    class HlenCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'hlen' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            return RespEncoder::encode_integer(static_cast<int64_t>(result.value().hash_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<HlenCommand>(*this);
        }
    };

    /**
     * @brief HgetallCommand - HGETALL 命令实现
     *
     * HGETALL 命令：获取所有哈希字段和值
     * 语法：HGETALL key
     * 返回：键值对列表
     */
    class HgetallCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'hgetall' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_array({});
            }

            auto items = result.value().hash_items();
            std::vector<std::string> response;
            for (const auto& [field, value] : items) {
                response.push_back(field);
                response.push_back(value);
            }

            return RespEncoder::encode_array(response);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<HgetallCommand>(*this);
        }
    };

    // ==================== Set 命令 ====================

    /**
     * @brief SaddCommand - SADD 命令实现
     *
     * SADD 命令：添加集合成员
     * 语法：SADD key member [member ...]
     * 返回：新增成员数量
     */
    class SaddCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() < 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'sadd' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            CacheObject obj;
            if (result.has_value()) {
                obj = std::move(result.value());
            }

            size_t added = 0;
            for (size_t i = 2; i < args.size(); ++i) {
                if (obj.set_add(args[i])) {
                    added++;
                }
            }

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(static_cast<int64_t>(added));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SaddCommand>(*this);
        }
    };

    /**
     * @brief SpopCommand - SPOP 命令实现
     *
     * SPOP 命令：随机弹出集合成员
     * 语法：SPOP key
     * 返回：弹出的成员或 nil
     */
    class SpopCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'spop' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            auto members = result.value().set_members();
            if (members.empty()) {
                return RespEncoder::encode_nil();
            }

            // 使用高质量随机数生成器
            static thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> dist(0, members.size() - 1);
            size_t idx = dist(rng);

            const std::string& member = members[idx];
            auto obj = std::move(result.value());
            obj.set_remove(member);

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_bulk_string(member);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SpopCommand>(*this);
        }
    };

    /**
     * @brief ScardCommand - SCARD 命令实现
     *
     * SCARD 命令：获取集合成员数量
     * 语法：SCARD key
     * 返回：成员数量
     */
    class ScardCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'scard' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            return RespEncoder::encode_integer(static_cast<int64_t>(result.value().set_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ScardCommand>(*this);
        }
    };

    /**
     * @brief SismemberCommand - SISMEMBER 命令实现
     *
     * SISMEMBER 命令：检查成员是否在集合中
     * 语法：SISMEMBER key member
     * 返回：1 在，0 不在
     */
    class SismemberCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'sismember' command");
            }

            const std::string& key = args[1];
            const std::string& member = args[2];

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            return RespEncoder::encode_integer(result.value().set_contains(member) ? 1 : 0);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SismemberCommand>(*this);
        }
    };

    /**
     * @brief SmembersCommand - SMEMBERS 命令实现
     *
     * SMEMBERS 命令：获取所有集合成员
     * 语法：SMEMBERS key
     * 返回：成员列表
     */
    class SmembersCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'smembers' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_array({});
            }

            auto members = result.value().set_members();
            return RespEncoder::encode_array(members);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SmembersCommand>(*this);
        }
    };

    // ==================== ZSet 命令 ====================

    /**
     * @brief ZaddCommand - ZADD 命令实现
     *
     * ZADD 命令：添加有序集合成员
     * 语法：ZADD key score member
     * 返回：新增成员数量
     */
    class ZaddCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 4) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zadd' command");
            }

            const std::string& key = args[1];
            double score;
            try {
                score = std::stod(args[2]);
            } catch (...) {
                return RespEncoder::encode_error("ERR invalid score");
            }
            const std::string& member = args[3];

            auto result = GlobalStorage::instance().get(key);

            CacheObject obj;
            if (result.has_value()) {
                obj = std::move(result.value());
            }

            bool is_new = obj.zset_add(member, score);

            GlobalStorage::instance().set(key, obj);
            return RespEncoder::encode_integer(is_new ? 1 : 0);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ZaddCommand>(*this);
        }
    };

    /**
     * @brief ZscoreCommand - ZSCORE 命令实现
     *
     * ZSCORE 命令：获取成员的分数
     * 语法：ZSCORE key member
     * 返回：分数或 nil
     */
    class ZscoreCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zscore' command");
            }

            const std::string& key = args[1];
            const std::string& member = args[2];

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_nil();
            }

            auto score = result.value().zset_score(member);
            if (!score) {
                return RespEncoder::encode_nil();
            }

            return RespEncoder::encode_bulk_string(std::to_string(score.value()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ZscoreCommand>(*this);
        }
    };

    /**
     * @brief ZcardCommand - ZCARD 命令实现
     *
     * ZCARD 命令：获取有序集合成员数量
     * 语法：ZCARD key
     * 返回：成员数量
     */
    class ZcardCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zcard' command");
            }

            const std::string& key = args[1];
            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_integer(0);
            }

            return RespEncoder::encode_integer(static_cast<int64_t>(result.value().zset_size()));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ZcardCommand>(*this);
        }
    };

    /**
     * @brief ZrangeCommand - ZRANGE 命令实现
     *
     * ZRANGE 命令：按分数范围获取成员
     * 语法：ZRANGE key min max
     * 返回：成员列表
     */
    class ZrangeCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 4 && args.size() != 5) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zrange' command");
            }

            const std::string& key = args[1];
            double min, max;
            try {
                min = std::stod(args[2]);
                max = std::stod(args[3]);
            } catch (...) {
                return RespEncoder::encode_error("ERR invalid score");
            }

            bool with_scores = (args.size() == 5 && args[4] == "WITHSCORES");

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_array({});
            }

            auto members = result.value().zset_range_by_score(min, max, with_scores);
            std::vector<std::string> response;
            for (const auto& [member, score] : members) {
                response.push_back(member);
                if (with_scores) {
                    response.push_back(std::to_string(score));
                }
            }

            return RespEncoder::encode_array(response);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<ZrangeCommand>(*this);
        }
    };

    class SaveCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            (void)args; // unused
            static std::string dump_path = "./dump.rdb";
            auto& rdb = RdbPersistence::instance();

            // 检查是否正在保存
            if (rdb.is_bgsave_in_progress()) {
                return RespEncoder::encode_error("ERR BGSAVE already in progress");
            }

            if (rdb.save(dump_path, GlobalStorage::instance())) {
                return RespEncoder::encode_simple_string("OK");
            }
            return RespEncoder::encode_error("ERR failed to save RDB");
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SaveCommand>();
        }
    };

    class BgsaveCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            (void)args; // unused
            static std::string dump_path = "./dump.rdb";
            auto& rdb = RdbPersistence::instance();

            // 检查是否正在保存
            if (rdb.is_bgsave_in_progress()) {
                return RespEncoder::encode_error("ERR BGSAVE already in progress");
            }

            if (rdb.save_in_background(dump_path, GlobalStorage::instance())) {
                return RespEncoder::encode_simple_string("Background saving started");
            }
            return RespEncoder::encode_error("ERR bgsave failed");
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<BgsaveCommand>();
        }
    };

    // LASTSAVE 命令 - 返回上次成功 RDB 保存的 Unix 时间戳
    class LastsaveCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            (void)args; // unused
            auto& rdb = RdbPersistence::instance();
            int64_t lastsave = rdb.get_last_bgsave_time();
            return RespEncoder::encode_integer(lastsave);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<LastsaveCommand>();
        }
    };

    // DBSIZE 命令 - 返回当前数据库的键数量
    class DbsizeCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            (void)args; // unused
            size_t size = GlobalStorage::instance().size();
            return RespEncoder::encode_integer(static_cast<int64_t>(size));
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DbsizeCommand>();
        }
    };

    // FLUSHDB 命令 - 清空当前数据库
    class FlushdbCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            (void)args; // unused
            GlobalStorage::instance().clear();
            return RespEncoder::encode_simple_string("OK");
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<FlushdbCommand>();
        }
    };

    // INFO 命令 - 返回服务器信息和统计
    class InfoCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            auto& rdb = RdbPersistence::instance();
            std::string section = (args.size() >= 2) ? args[1] : "server";

            std::string result;
            if (section == "server" || section == "all") {
                result += "# Server\r\n";
                result += "concurrentcache_version:3.0.0\r\n";
                result += "os:Linux\r\n";
                result += "arch_bits:64\r\n";
            }

            if (section == "stats" || section == "all") {
                result += "# Stats\r\n";
                auto& stats = rdb.get_stats();
                result += "total_connections_received:0\r\n";
                result += "total_commands_processed:0\r\n";
                result += "total_bgsave_calls:" + std::to_string(stats.total_bgsave_calls.load()) + "\r\n";
                result += "total_rdb_saved_keys:" + std::to_string(stats.total_rdb_saved_keys.load()) + "\r\n";
            }

            if (section == "persistence" || section == "all") {
                result += "# Persistence\r\n";
                auto& stats = rdb.get_stats();
                result += "rdb_enabled:yes\r\n";
                result += "rdb_last_bgsave_status:" +
                    std::string(stats.last_bgsave_status.load() == BgsaveStatus::SUCCESS ? "ok" :
                     stats.last_bgsave_status.load() == BgsaveStatus::FAILED ? "err" : "idle") + "\r\n";
                result += "rdb_last_bgsave_time_sec:" + std::to_string(stats.last_bgsave_time_sec.load()) + "\r\n";
                result += "rdb_last_bgsave_keys:" + std::to_string(stats.last_bgsave_keys.load()) + "\r\n";
                result += "rdb_dirty_count:" + std::to_string(GlobalStorage::instance().get_dirty_count()) + "\r\n";
            }

            if (section == "keyspace" || section == "all") {
                result += "# Keyspace\r\n";
                size_t db_size = GlobalStorage::instance().size();
                result += "db0:keys=" + std::to_string(db_size) + ",expires=0,avg_ttl=0\r\n";
            }

            if (result.empty()) {
                return RespEncoder::encode_error("ERR Unknown INFO section: " + section);
            }

            return RespEncoder::encode_bulk_string(result);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<InfoCommand>();
        }
    };

    // DEBUG 命令 - 调试命令（简化实现）
    class DebugCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() < 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'debug' command");
            }

            const std::string& subcommand = args[1];

            if (subcommand == "sleep") {
                if (args.size() < 3) {
                    return RespEncoder::encode_error("ERR wrong number of arguments for 'debug sleep'");
                }
                try {
                    double seconds = std::stod(args[2]);
                    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
                } catch (...) {
                    return RespEncoder::encode_error("ERR invalid sleep time");
                }
                return RespEncoder::encode_simple_string("OK");
            }

            if (subcommand == "object") {
                if (args.size() < 3) {
                    return RespEncoder::encode_error("ERR wrong number of arguments for 'debug object'");
                }
                const std::string& key = args[2];
                auto result = GlobalStorage::instance().get(key);
                if (!result.has_value()) {
                    return RespEncoder::encode_error("ERR no such key");
                }
                const CacheObject& obj = result.value();
                std::string info = "Type: ";
                switch (obj.type()) {
                    case ObjectType::STRING: info += "string"; break;
                    case ObjectType::LIST: info += "list"; break;
                    case ObjectType::HASH: info += "hash"; break;
                    case ObjectType::SET: info += "set"; break;
                    case ObjectType::ZSET: info += "zset"; break;
                    default: info += "unknown"; break;
                }
                return RespEncoder::encode_bulk_string(info);
            }

            return RespEncoder::encode_error("ERR Unknown DEBUG subcommand");
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DebugCommand>();
        }
    };
}

#endif
