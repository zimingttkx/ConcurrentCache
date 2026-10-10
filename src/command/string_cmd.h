#ifndef CONCURRENTCACHE_COMMAND_STRING_CMD_H
#define CONCURRENTCACHE_COMMAND_STRING_CMD_H

#include "command.h"
#include "cache/storage.h"
#include "protocol/resp.h"
#include "datatype/object.h"
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <charconv>
#include <limits>
#include <random>

#include "persistence/rdb.h"
#include "base/server_stats.h"

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
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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
            // SET key value [EX 秒 | PX 毫秒 | EXAT unix秒 | PXAT unix毫秒 | KEEPTTL]
            //             [NX | XX] [GET]
            //
            // 以前这里只认 args.size() == 3，多余参数一律 wrong number of arguments，
            // 于是 redis-py 的 set(..., ex=…)、Jedis 的 SetParams 全都打不进来。
            if (args.size() < 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'set' command");
            }

            const std::string& key = args[1];
            const std::string& value = args[2];

            enum class ExpMode { kNone, kEx, kPx, kExat, kPxat };
            ExpMode mode = ExpMode::kNone;
            long long raw = 0;
            bool nx = false, xx = false, want_get = false, keep_ttl = false;

            for (size_t i = 3; i < args.size(); ++i) {
                const std::string opt = to_upper(args[i]);   // Redis 的 SET 选项大小写不敏感
                if (opt == "EX" || opt == "PX" || opt == "EXAT" || opt == "PXAT") {
                    if (mode != ExpMode::kNone || keep_ttl) {
                        return RespEncoder::encode_error("ERR syntax error");  // 只能指定一种过期方式
                    }
                    if (i + 1 >= args.size()) {
                        return RespEncoder::encode_error("ERR syntax error");
                    }
                    if (!parse_ll(args[++i], raw)) {
                        return RespEncoder::encode_error("ERR value is not an integer or out of range");
                    }
                    mode = (opt == "EX")     ? ExpMode::kEx
                           : (opt == "PX")   ? ExpMode::kPx
                           : (opt == "EXAT") ? ExpMode::kExat
                                             : ExpMode::kPxat;
                } else if (opt == "NX") {
                    if (xx || nx) return RespEncoder::encode_error("ERR syntax error");
                    nx = true;
                } else if (opt == "XX") {
                    if (xx || nx) return RespEncoder::encode_error("ERR syntax error");
                    xx = true;
                } else if (opt == "GET") {
                    want_get = true;
                } else if (opt == "KEEPTTL") {
                    if (mode != ExpMode::kNone || keep_ttl) {
                        return RespEncoder::encode_error("ERR syntax error");
                    }
                    keep_ttl = true;
                } else {
                    return RespEncoder::encode_error("ERR syntax error");
                }
            }

            // Redis 明确拒绝 NX 与 GET 同时出现（两者对"没写成功"的回复互相矛盾）。
            // 文案没对真 Redis 核过，所以用例只断言它必须报错。
            if (nx && want_get) {
                return RespEncoder::encode_error("ERR syntax error");
            }

            const int64_t now = GlobalStorage::instance().current_time_ms();
            constexpr long long kMax = std::numeric_limits<long long>::max();
            int64_t ttl_ms = 0;
            bool already_expired = false;
            if (mode == ExpMode::kEx || mode == ExpMode::kPx) {
                if (raw <= 0) {
                    return RespEncoder::encode_error("ERR invalid expire time in 'set' command");
                }
                if (mode == ExpMode::kEx && raw > kMax / 1000) {
                    return RespEncoder::encode_error("ERR invalid expire time in 'set' command");
                }
                ttl_ms = (mode == ExpMode::kEx) ? raw * 1000 : raw;
            } else if (mode == ExpMode::kExat || mode == ExpMode::kPxat) {
                if (raw < 0) {
                    return RespEncoder::encode_error("ERR invalid expire time in 'set' command");
                }
                const int64_t abs_ms = (mode == ExpMode::kExat)
                                           ? (raw > kMax / 1000 ? kMax : raw * 1000)
                                           : raw;
                if (abs_ms <= now) {
                    already_expired = true;   // 绝对时刻已经过去：SET 不写值，改成删键
                } else {
                    ttl_ms = abs_ms - now;    // 存储层只吃相对时长
                }
            }

            const bool wrong_type = want_get && is_non_string(key);
            if (wrong_type) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }

            if (already_expired) {
                std::optional<CacheObject> cur = GlobalStorage::instance().get(key);
                const bool present = cur.has_value();
                if ((nx && present) || (xx && !present)) {
                    return RespEncoder::encode_nil();   // 条件不满足：一个字节都不动
                }
                GlobalStorage::instance().del(key);
                if (want_get) {
                    if (!present) return RespEncoder::encode_nil();
                    return RespEncoder::encode_bulk_string(cur->get_string().value_or(""));
                }
                return RespEncoder::encode_ok();
            }

            const SetCondition condition = nx ? SetCondition::kOnlyIfAbsent
                                              : (xx ? SetCondition::kOnlyIfExists
                                                    : SetCondition::kNone);
            std::optional<CacheObject> previous;
            const bool applied = GlobalStorage::instance().set_conditional(
                key, CacheObject(value), condition, keep_ttl ? 0 : ttl_ms, keep_ttl, previous);

            if (want_get) {
                if (!applied || !previous) return RespEncoder::encode_nil();
                return RespEncoder::encode_bulk_string(previous->get_string().value_or(""));
            }
            return applied ? RespEncoder::encode_ok() : RespEncoder::encode_nil();
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<SetCommand>(*this);
        }

    private:
        static std::string to_upper(const std::string& s) {
            std::string out = s;
            // toupper 的入参必须是 unsigned char（负数 char 是 UB），这里显式转一次：
            // -Werror=sign-conversion 不接受隐式的 char→unsigned char。
            for (char& ch : out) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            return out;
        }

        // 严格整数：整个串都被吃掉才算数。用 from_chars 而不是 stoll —— 后者读前缀
        // 就返回（"10abc" 给出 10），溢出还得靠异常兜。
        static bool parse_ll(const std::string& s, long long& out) {
            const char* first = s.data();
            const char* last = first + s.size();
            const std::from_chars_result r = std::from_chars(first, last, out);
            return r.ec == std::errc() && r.ptr == last;
        }

        // SET GET 要求旧值是字符串类型。这一步和后面的写入之间有一次取锁间隙：
        // 并发下最坏结果是"这轮判成 WRONGTYPE、并没有写"，不会丢数据也不会写错值。
        // 要彻底收口得让 set_conditional 带类型回调，不在这个改动里做。
        static bool is_non_string(const std::string& key) {
            std::optional<CacheObject> cur = GlobalStorage::instance().get(key);
            return cur.has_value() && !cur->get_string().has_value();
        }
    };

    /**
     * @brief IncrCommand - INCR 命令实现
     *
     * INCR 命令：对指定 key 的值进行原子加 1
     * 语法：INCR key
     * 返回：递增后的整数值
     */
    // INCR / DECR / INCRBY / DECRBY 共用一条路径。
    // storage.incrby 在分片独占锁内做原子读-改-写；返回 nullopt 时再取一次值，
    // 用来区分"是字符串但不是整数"和"根本不是字符串"这两种不同的错误。
    inline std::string apply_incrby(const std::string& key, int64_t delta) {
        auto& storage = GlobalStorage::instance();
        auto result = storage.incrby(key, delta);
        if (result.has_value()) {
            return RespEncoder::encode_integer(result.value());
        }

        auto opt = storage.get(key);
        if (opt.has_value() && opt.value().get_string().has_value()) {
            try {
                std::stoll(opt.value().get_string().value());
            } catch (...) {
                return RespEncoder::encode_error("ERR value is not an integer");
            }
            return RespEncoder::encode_error("ERR increment or decrement would overflow");
        }
        return RespEncoder::encode_error(
            "WRONGTYPE Operation against a key holding the wrong kind of value");
    }

    class IncrCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() != 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'incr' command");
            }

            return apply_incrby(args[1], 1);
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

            return apply_incrby(args[1], -1);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DecrCommand>(*this);
        }
    };

    /**
     * @brief IncrbyCommand - INCRBY 命令实现
     *
     * INCRBY key delta
     */
    class IncrbyCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'incrby' command");
            }

            long long delta = 0;
            try {
                delta = std::stoll(args[2]);
            } catch (...) {
                return RespEncoder::encode_error("ERR value is not an integer or out of range");
            }
            return apply_incrby(args[1], delta);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<IncrbyCommand>(*this);
        }
    };

    /**
     * @brief DecrbyCommand - DECRBY 命令实现
     *
     * DECRBY key delta
     */
    class DecrbyCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            if (args.size() != 3) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'decrby' command");
            }

            long long delta = 0;
            try {
                delta = std::stoll(args[2]);
            } catch (...) {
                return RespEncoder::encode_error("ERR value is not an integer or out of range");
            }
            if (delta == std::numeric_limits<long long>::min()) {
                // 取负会溢出；而"减去 -2^63"本身就等于加 2^63，超出行范围
                return RespEncoder::encode_error("ERR value is not an integer or out of range");
            }
            return apply_incrby(args[1], -delta);
        }

        [[nodiscard]] std::unique_ptr<Command> clone() const override {
            return std::make_unique<DecrbyCommand>(*this);
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
            // 参数验证：DEL 命令需要至少 2 个参数 [命令名, key...]
            if (args.size() < 2) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'del' command");
            }

            size_t deleted = 0;
            for (size_t i = 1; i < args.size(); ++i) {
                if (GlobalStorage::instance().del(args[i])) {
                    deleted++;
                }
            }

            return RespEncoder::encode_integer(static_cast<int64_t>(deleted));
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
            int64_t new_len = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::LIST, [&](CacheObject& obj) {
                    for (size_t i = 2; i < args.size(); ++i) {
                        obj.list_push(args[i], true);
                    }
                    new_len = static_cast<int64_t>(obj.list_size());
                    return StoreOp::kWrite;
                });
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(new_len);
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
            int64_t new_len = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::LIST, [&](CacheObject& obj) {
                    for (size_t i = 2; i < args.size(); ++i) {
                        obj.list_push(args[i], false);
                    }
                    new_len = static_cast<int64_t>(obj.list_size());
                    return StoreOp::kWrite;
                });
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(new_len);
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
            std::optional<std::string> popped;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::LIST, [&](CacheObject& obj) {
                    auto val = obj.list_pop(true);
                    if (!val) return StoreOp::kNoop;
                    popped = val;
                    // 弹空就删键：留一个空列表在库里，EXISTS / DBSIZE 都会多算一个
                    return obj.list_size() == 0 ? StoreOp::kErase : StoreOp::kWrite;
                },
                StringPromotion::kReject);
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            if (!popped) {
                return RespEncoder::encode_nil();
            }
            return RespEncoder::encode_bulk_string(popped.value());
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
            std::optional<std::string> popped;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::LIST, [&](CacheObject& obj) {
                    auto val = obj.list_pop(false);
                    if (!val) return StoreOp::kNoop;
                    popped = val;
                    return obj.list_size() == 0 ? StoreOp::kErase : StoreOp::kWrite;
                },
                StringPromotion::kReject);
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            if (!popped) {
                return RespEncoder::encode_nil();
            }
            return RespEncoder::encode_bulk_string(popped.value());
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

            if (result.value().type() != ObjectType::LIST) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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

            if (result.value().type() != ObjectType::LIST) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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

            int64_t added = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::HASH, [&](CacheObject& obj) {
                    added = obj.hash_exists(field) ? 0 : 1;
                    obj.hash_set(field, value);
                    return StoreOp::kWrite;
                });
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(added);
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

            if (result.value().type() != ObjectType::HASH) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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
            int64_t deleted = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::HASH, [&](CacheObject& obj) {
                    for (size_t i = 2; i < args.size(); ++i) {
                        if (obj.hash_del(args[i])) {
                            ++deleted;
                        }
                    }
                    if (deleted == 0) return StoreOp::kNoop;
                    // 最后一个字段删掉之后键必须消失，不能留一张空哈希
                    return obj.hash_size() == 0 ? StoreOp::kErase : StoreOp::kWrite;
                },
                StringPromotion::kReject);
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(deleted);
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

            if (result.value().type() != ObjectType::HASH) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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

            if (result.value().type() != ObjectType::HASH) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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
            int64_t added = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::SET, [&](CacheObject& obj) {
                    for (size_t i = 2; i < args.size(); ++i) {
                        if (obj.set_add(args[i])) {
                            ++added;
                        }
                    }
                    // 全是已有成员时什么都没变，不必写回也不该动脏计数
                    return added == 0 ? StoreOp::kNoop : StoreOp::kWrite;
                });
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(added);
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
            std::optional<std::string> popped;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::SET, [&](CacheObject& obj) {
                    const auto members = obj.set_members();
                    if (members.empty()) return StoreOp::kNoop;

                    // 使用高质量随机数生成器
                    static thread_local std::mt19937 rng(std::random_device{}());
                    std::uniform_int_distribution<size_t> dist(0, members.size() - 1);
                    const size_t idx = dist(rng);

                    const std::string member = members[idx];
                    obj.set_remove(member);
                    popped = member;
                    // 弹空即删键
                    return obj.set_size() == 0 ? StoreOp::kErase : StoreOp::kWrite;
                },
                StringPromotion::kReject);
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            if (!popped) {
                return RespEncoder::encode_nil();
            }
            return RespEncoder::encode_bulk_string(popped.value());
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

            if (result.value().type() != ObjectType::SET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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

            if (result.value().type() != ObjectType::SET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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

            if (result.value().type() != ObjectType::SET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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
            // ZADD key score member [score member ...]
            if (args.size() < 4 || (args.size() - 2) % 2 != 0) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zadd' command");
            }

            const std::string& key = args[1];

            // 先把 score 全部解析完再改库：Redis 的语义是任一 score 非法则整条
            // 命令不做任何改动。放在 mutate 的回调里做不了这件事（回调 return 不
            // 到外层函数），而且解析失败时键已经被凭空建出来了。
            std::vector<std::pair<std::string, double>> entries;
            entries.reserve((args.size() - 2) / 2);
            for (size_t i = 2; i < args.size(); i += 2) {
                double score;
                try {
                    size_t consumed = 0;
                    score = std::stod(args[i], &consumed);
                    // std::stod 读前缀就返回："1.5abc" 给 1.5 且不抛。Redis 的
                    // string2d 要求整个串都被吃掉，否则就是把客户端的拼写错误
                    // 静默收成一个并不存在的数。
                    if (consumed != args[i].size()) {
                        return RespEncoder::encode_error("ERR value is not a valid float");
                    }
                } catch (...) {
                    // 完全读不出来，或者超范围（1e400 → out_of_range）
                    return RespEncoder::encode_error("ERR value is not a valid float");
                }
                // nan / inf 是合法浮点字面量，stod 照收。Redis 明确拒绝：sorted
                // set 靠分数排全序，NaN 跟谁比都不成立，收进来之后这张表的顺序
                // 就不再确定（而且回复里会出现一个再也读不回同值的字符串）。
                if (std::isnan(score) || std::isinf(score)) {
                    return RespEncoder::encode_error("ERR value is NaN or Infinity");
                }
                entries.emplace_back(args[i + 1], score);
            }

            int64_t added = 0;
            const bool ok = GlobalStorage::instance().mutate(
                key, ObjectType::ZSET, [&](CacheObject& obj) {
                    for (const auto& entry : entries) {
                        if (obj.zset_add(entry.first, entry.second)) {
                            ++added;
                        }
                    }
                    return added == 0 ? StoreOp::kNoop : StoreOp::kWrite;
                });
            if (!ok) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return RespEncoder::encode_integer(added);
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

            if (result.value().type() != ObjectType::ZSET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }

            auto score = result.value().zset_score(member);
            if (!score) {
                return RespEncoder::encode_nil();
            }

            return RespEncoder::encode_bulk_string(RespEncoder::format_double(score.value()));
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

            if (result.value().type() != ObjectType::ZSET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
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
     * ZRANGE 命令：按索引范围获取成员（Redis 语义：start/stop 是排名索引）
     * 语法：ZRANGE key start stop [WITHSCORES]
     * 返回：成员列表
     */
    class ZrangeCommand : public Command {
    public:
        std::string execute(const std::vector<std::string> &args) override {
            if (args.size() != 4 && args.size() != 5) {
                return RespEncoder::encode_error("ERR wrong number of arguments for 'zrange' command");
            }

            const std::string& key = args[1];
            long long start, stop;
            try {
                start = std::stoll(args[2]);
                stop = std::stoll(args[3]);
            } catch (...) {
                return RespEncoder::encode_error("ERR invalid integer");
            }

            bool with_scores = (args.size() == 5 && args[4] == "WITHSCORES");

            auto result = GlobalStorage::instance().get(key);

            if (!result.has_value()) {
                return RespEncoder::encode_array({});
            }

            if (result.value().type() != ObjectType::ZSET) {
                return RespEncoder::encode_error(
                    "WRONGTYPE Operation against a key holding the wrong kind of value");
            }

            auto members = result.value().zset_range_by_index(start, stop, with_scores);
            std::vector<std::string> response;
            for (const auto& [member, score] : members) {
                response.push_back(member);
                if (with_scores) {
                    response.push_back(RespEncoder::format_double(score));
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
            // 修复 P1-13：使用配置中的 rdb_path，而非硬编码 ./dump.rdb，避免手动 save
            // 写入错误路径、重启加载旧配置路径导致数据丢失。
            std::string dump_path = Config::instance().getString("rdb_path", "./dump.rdb");
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
            std::string dump_path = Config::instance().getString("rdb_path", "./dump.rdb");
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
    // 进程当前常驻内存（字节）。
    //
    // 只报内核知道的量：/proc/self/status 的 VmRSS。项目里那三层内存池还
    // 没接进任何分配路径，所以**没有**可信的"分配器已用字节数"可报 ——
    // Redis 的 used_memory 那一栏故意不出，宁可让工具看到字段缺失，也不要
    // 一个抄来的数字看着像真的。读不到（非 Linux / proc 不可用）时返回 0，
    // 由调用方如实报 0，不做任何猜测。
    // 从一行 /proc/self/status 里取 VmRSS 的 kB 并换成字节。
    // 单独拆出来是为了能被单元测试直接钉住（"VmRSS:\t1024 kB" 必须是 1048576，
    // 没有数字的那行必须是 0，而不是一个没定义的返回值）。
    // inline：这个头文件会被多个 TU 包含，static 会让每个 TU 各留一份。
    inline uint64_t rss_bytes_from_status_line(const std::string& line) {
        if (line.compare(0, 6, "VmRSS:") != 0) {
            return 0;
        }
        const std::size_t begin = line.find_first_of("0123456789");
        if (begin == std::string::npos || begin == line.size()) {
            return 0;
        }
        const char* first = line.c_str() + begin;
        char* last = nullptr;
        const unsigned long long kb = std::strtoull(first, &last, 10);
        if (last == first) {
            return 0;
        }
        return static_cast<uint64_t>(kb) * 1024ull;
    }

    inline uint64_t process_rss_bytes() {
#ifdef __linux__
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.compare(0, 6, "VmRSS:") == 0) {
                return rss_bytes_from_status_line(line);
            }
        }
#endif
        return 0;
    }

    class InfoCommand : public Command {
    public:
        std::string execute(const std::vector<std::string>& args) override {
            auto& rdb = RdbPersistence::instance();
            std::string section = (args.size() >= 2) ? args[1] : "server";

            std::string result;
            if (section == "server" || section == "all") {
                result += "# Server\r\n";
                result += "concurrentcache_version:4.0.0\r\n";
#if defined(__linux__)
                result += "os:Linux\r\n";
#elif defined(_WIN32)
                result += "os:Windows\r\n";
#else
                result += "os:Unknown\r\n";
#endif
                result += "arch_bits:" + std::to_string(sizeof(void*) * 8) + "\r\n";
            }

            if (section == "stats" || section == "all") {
                result += "# Stats\r\n";
                auto& stats = rdb.get_stats();
                auto& counters = ServerStats::instance();
                result += "total_connections_received:"
                          + std::to_string(counters.connections_accepted()) + "\r\n";
                result += "total_commands_processed:"
                          + std::to_string(counters.commands_processed()) + "\r\n";
                result += "total_bgsave_calls:" + std::to_string(stats.total_bgsave_calls.load()) + "\r\n";
                result += "total_rdb_saved_keys:" + std::to_string(stats.total_rdb_saved_keys.load()) + "\r\n";
            }

            if (section == "memory" || section == "all") {
                result += "# Memory\r\n";
                const uint64_t rss = process_rss_bytes();
                result += "used_memory_rss:" + std::to_string(rss) + "\r\n";
                result += "used_memory_rss_human:"
                          + std::to_string(rss / (1024ull * 1024ull)) + "MB\r\n";
                // 淘汰只看条数不看字节（没有 Redis 那种按字节的 maxmemory 口径），
                // 所以这里给的是"按什么在淘汰"，不是一个假装存在的字节上限。
                result += "maxmemory:0\r\n";
                result += "maxmemory_human:0B\r\n";
                result += "maxmemory_policy:aru-random-shard-sampling\r\n";
                result += "used_memory_keys:"
                          + std::to_string(GlobalStorage::instance().size()) + "\r\n";
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
                // keys 是底层哈希表的条目数，**含**已过期但还没被删的键（size() 不做
                // 过期过滤）。expires / avg_ttl 只数还没过期的那些，所以完全可能出现
                // keys=100 / expires=3 这种组合 —— 两个字段量的不是同一批键。
                size_t db_size = GlobalStorage::instance().size();
                size_t live_expires = 0;
                int64_t avg_ttl_ms = 0;
                GlobalStorage::instance().expire_dict().stats(live_expires, avg_ttl_ms);
                result += "db0:keys=" + std::to_string(db_size)
                          + ",expires=" + std::to_string(live_expires)
                          + ",avg_ttl=" + std::to_string(avg_ttl_ms) + "\r\n";
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
                // 拒绝 DEBUG SLEEP：它会同步阻塞当前 SubReactor 的事件循环，
                // 冻结该 reactor 上的所有客户端（修复 P1-9）。Redis 的实现也是异步/测试专用。
                return RespEncoder::encode_error("ERR DEBUG SLEEP is not supported");
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
