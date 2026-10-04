# ConcurrentCache API 参考

> **协议**：Redis RESP 2.0（兼容任意 Redis 客户端：`redis-cli`、`jedis`、`redis-py`、`go-redis`）
> **默认端口**：`16379`（`conf/concurrentcache.conf` 中 `port` 项可改）
> **命令总数**：44 个（注册于 `src/command/command_factory.cpp`）
> **文档维护**：与命令注册表严格同步，修改注册表必须同步本文档

## 1. 快速开始

```bash
$ redis-cli -p 16379 PING
PONG
```

```python
# Python (redis-py)
import redis
r = redis.Redis(host='127.0.0.1', port=16379, decode_responses=True)
r.set('greeting', 'Hello, ConcurrentCache')
print(r.get('greeting'))   # Hello, ConcurrentCache
```

## 2. 命令索引

| 分类 | 命令数 | 命令 |
|------|--------|------|
| [§ 3 连接](#3-连接) | 1 | PING |
| [§ 4 字符串](#4-字符串-string) | 6 | GET / SET / DEL / EXISTS / INCR / DECR |
| [§ 5 过期](#5-过期-ttl) | 5 | EXPIRE / TTL / PTTL / PERSIST / SETEX |
| [§ 6 列表](#6-列表-list) | 6 | LPUSH / RPUSH / LPOP / RPOP / LLEN / LRANGE |
| [§ 7 哈希](#7-哈希-hash) | 5 | HSET / HGET / HDEL / HLEN / HGETALL |
| [§ 8 集合](#8-集合-set) | 5 | SADD / SPOP / SCARD / SISMEMBER / SMEMBERS |
| [§ 9 有序集合](#9-有序集合-zset) | 4 | ZADD / ZSCORE / ZCARD / ZRANGE |
| [§ 10 持久化](#10-持久化) | 5 | SAVE / BGSAVE / LASTSAVE / DBSIZE / FLUSHDB |
| [§ 11 服务器](#11-服务器) | 2 | INFO / DEBUG |
| [§ 12 集群](#12-集群) | 1 | CLUSTER（含 10 个子命令） |
| [§ 13 复制](#13-复制) | 3 | PSYNC / SYNC / REPLCONF |
| [§ 14 迁移](#14-迁移) | 1 | RESTORE |

## 3. 连接

### PING

```text
PING [message]
```

- 无参数 → `+PONG\r\n`
- 有参数 → 回显消息（Bulk String）

## 4. 字符串 String

| 命令 | 语法 | 返回 | 复杂度 |
|------|------|------|--------|
| `GET` | `GET key` | `$N\r\nvalue\r\n` 或 `$-1\r\n` | O(1) |
| `SET` | `SET key value` | `+OK\r\n`（覆盖写入并清除该 key 已有的 TTL，等同 PERSIST） | O(1) |
| `DEL` | `DEL key [key ...]` | `:N\r\n`（删除成功的 key 数） | O(N) |
| `EXISTS` | `EXISTS key` | `:1\r\n` 或 `:0\r\n`（已过期但尚未被删除的 key 返回 0） | O(1) |
| `INCR` | `INCR key` | 递增后整数值（原子） | O(1) |
| `DECR` | `DECR key` | 递减后整数值（原子） | O(1) |

> **DEL** 支持批量删除多个 key，返回实际删除成功的数量。
> **INCR/DECR** 是**原子操作**（读-改-写在分片独占锁内完成，并发调用不丢失更新）：若 key 不存在视为 0；若值非整数返回 `-ERR value is not an integer`；若 64 位递增/递减溢出返回 `-ERR increment or decrement would overflow`；若 key 持有非 STRING 类型返回 `-WRONGTYPE Operation against a key holding the wrong kind of value`。

## 5. 过期 TTL

| 命令 | 语法 | 返回 |
|------|------|------|
| `EXPIRE` | `EXPIRE key seconds` | `:1\r\n`（成功，或 seconds≤0 时按"立即过期"删掉该键）/ `:0\r\n`（key 不存在） |
| `TTL` | `TTL key` | 剩余秒数；`-1`=永不过期；`-2`=不存在 |
| `PTTL` | `PTTL key` | 剩余毫秒数；语义同上 |
| `PERSIST` | `PERSIST key` | `:1\r\n` / `:0\r\n`（成功/无过期或不存在） |
| `SETEX` | `SETEX key seconds value` | `+OK\r\n`（原子设置值+TTL） |

## 6. 列表 List

| 命令 | 语法 | 返回 |
|------|------|------|
| `LPUSH` | `LPUSH key value [value ...]` | 操作后列表长度 |
| `RPUSH` | `RPUSH key value [value ...]` | 操作后列表长度 |
| `LPOP` | `LPOP key` | 弹出的元素或 nil |
| `RPOP` | `RPOP key` | 弹出的元素或 nil |
| `LLEN` | `LLEN key` | 列表长度（不存在返回 0） |
| `LRANGE` | `LRANGE key start stop` | 元素数组（支持负索引） |

> **LPUSH/RPUSH** 支持一次推入多个值。若 key 当前为 STRING 类型，会自动转换为 LIST（空字符串不保留）。
> **LRANGE** 的 start/stop 支持负索引（-1 表示最后一个元素）。
> 对非 LIST 类型的 key 执行列表命令返回 `-WRONGTYPE` 错误。

## 7. 哈希 Hash

| 命令 | 语法 | 返回 |
|------|------|------|
| `HSET` | `HSET key field value` | `:1\r\n` 新增 / `:0\r\n` 更新 |
| `HGET` | `HGET key field` | 值或 nil |
| `HDEL` | `HDEL key field [field ...]` | 删除成功的字段数 |
| `HLEN` | `HLEN key` | 字段数 |
| `HGETALL` | `HGETALL key` | field/value 交替数组 |

> **HSET** 当前仅支持单个 field/value 对（与 Redis 4.0+ 的多对支持不同）。
> **HDEL** 支持一次删除多个字段。
> 对非 HASH 类型的 key 执行哈希命令返回 `-WRONGTYPE` 错误。

## 8. 集合 Set

| 命令 | 语法 | 返回 |
|------|------|------|
| `SADD` | `SADD key member [member ...]` | 新增成员数 |
| `SPOP` | `SPOP key` | 随机弹出的成员或 nil |
| `SCARD` | `SCARD key` | 成员数（不存在返回 0） |
| `SISMEMBER` | `SISMEMBER key member` | `:1\r\n` / `:0\r\n` |
| `SMEMBERS` | `SMEMBERS key` | 成员数组 |

> **SADD** 支持一次添加多个成员。
> **SPOP** 使用 `std::mt19937` 线程局部随机数生成器。
> 对非 SET 类型的 key 执行集合命令返回 `-WRONGTYPE` 错误。

## 9. 有序集合 ZSet

| 命令 | 语法 | 返回 |
|------|------|------|
| `ZADD` | `ZADD key score member [score member ...]` | 新增成员数 |
| `ZSCORE` | `ZSCORE key member` | 分数（Bulk String）或 nil |
| `ZCARD` | `ZCARD key` | 成员数 |
| `ZRANGE` | `ZRANGE key start stop [WITHSCORES]` | 按排名索引返回成员 |

> **ZADD** 支持一次添加多个 score/member 对。若 member 已存在且 score 不同，会更新分数。
> **ZRANGE** 按排名索引（index）范围查询，start/stop 支持负索引（-1 表示最后一个）。可选 `WITHSCORES` 参数同时返回分数。**不支持 Redis 6.2+ 的 BYSCORE/BYLEX/REV/LIMIT 选项**。
> 对非 ZSET 类型的 key 执行有序集合命令返回 `-WRONGTYPE` 错误。

## 10. 持久化

| 命令 | 语法 | 返回 |
|------|------|------|
| `SAVE` | `SAVE` | `+OK\r\n` / `-ERR failed to save RDB`（保存路径取 `rdb_path` 配置） |
| `BGSAVE` | `BGSAVE` | `+Background saving started\r\n` / `-ERR bgsave failed` |
| `LASTSAVE` | `LASTSAVE` | 上次成功保存的 Unix 时间戳（秒，整数） |
| `DBSIZE` | `DBSIZE` | 当前 key 数量（整数；已过期但尚未被删除的 key 不计入语义，见 EXISTS） |
| `FLUSHDB` | `FLUSHDB` | `+OK\r\n`（清空全部数据） |

> **BGSAVE** 进行中再次触发（含 SAVE）→ `-ERR BGSAVE already in progress`。RDB 保存为进程内后台线程快照，先写 `.tmp` 临时文件再原子 rename（详见[持久化架构](architecture/persistence.md)）。

## 11. 服务器

| 命令 | 语法 | 返回 |
|------|------|------|
| `INFO` | `INFO [section]` | Bulk String（server/stats/persistence/keyspace/all） |
| `DEBUG` | `DEBUG OBJECT <key>` | 类型信息（Bulk String，如 `Type: string`）；key 不存在返回 `-ERR no such key` |

> **DEBUG SLEEP 已被移除**：`DEBUG SLEEP <sec>` 会阻塞事件循环，现返回 `-ERR DEBUG SLEEP is not supported`。

`INFO` 输出示例：

```text
# Server
concurrentcache_version:3.0.0
os:Linux
arch_bits:64
# Stats
total_bgsave_calls:5
total_rdb_saved_keys:12345
# Persistence
rdb_last_bgsave_status:ok
rdb_last_bgsave_time_sec:1718700000
rdb_dirty_count:0
# Keyspace
db0:keys=12345,expires=0,avg_ttl=0
```

## 12. 集群

```text
CLUSTER <SUBCOMMAND> [arg ...]
```

| 子命令 | 语法 | 说明 |
|--------|------|------|
| `MEET` | `CLUSTER MEET <ip> <port>` | 加入新节点到集群 |
| `NODES` | `CLUSTER NODES` | 列出所有节点信息 |
| `INFO` | `CLUSTER INFO` | 集群状态摘要 |
| `ADDSLOTS` | `CLUSTER ADDSLOTS <slot> [slot ...]` | 指派槽到本节点 |
| `SLOTS` | `CLUSTER SLOTS` | 槽-节点映射 |
| `DELSLOTS` | `CLUSTER DELSLOTS <slot> [slot ...]` | 移除本节点槽 |
| `SETSLOT` | `CLUSTER SETSLOT <slot> NODE/MIGRATING/IMPORTING` | 设置槽状态 |
| `REPLICATE` | `CLUSTER REPLICATE <node-name>` | 将本节点设为某主节点的从节点 |
| `FAIL` | `CLUSTER FAIL` | 强制标记主节点下线 |
| `MIGRATE` | `CLUSTER MIGRATE ...` | 槽迁移（内部） |

**重定向响应**：

| 响应 | 含义 |
|------|------|
| `-MOVED <slot> <ip:port>` | 槽已迁出，客户端应永久重定向 |
| `-ASK <slot> <ip:port>` | 槽正在迁入，本次重定向即可 |

## 13. 复制

| 命令 | 语法 | 用途 |
|------|------|------|
| `PSYNC` | `PSYNC <runid> <offset>` / `PSYNC ? -1` | 主从同步（增量 / 全量） |
| `SYNC` | `SYNC` | 旧版同步（已废弃，保留兼容） |
| `REPLCONF` | `REPLCONF <key> <value>` | 复制配置（`listening-port` / `ack` / `getack`） |

`PSYNC ? -1` 触发全量同步（主发 RESTORE 键流，详见[集群架构](architecture/cluster.md)）；`PSYNC <runid> <offset>` 尝试增量。复制命令以 RESP 数组二进制安全传输。

## 14. 迁移

```text
RESTORE <key> <ttl> <serialized-value>
```

- 从 `CLUSTER MIGRATE` 流程接收已序列化的 `CacheObject`
- `ttl` 单位为毫秒；0 表示永不过期
- 配套序列化由 `CacheObject::serialize()` 提供
- 错误返回：`-ERR invalid TTL`（ttl 非法）/ `-ERR invalid serialized data for <TYPE>`（反序列化失败）/ `-BUSYKEY Target key name already exists`（key 已存在且未带 REPLACE）

## 15. 错误码

| RESP 响应 | 含义 |
|----------|------|
| `-ERR wrong number of arguments for '<cmd>' command` | 参数个数错误 |
| `-ERR value is not an integer` | 需整数参数但传入非数字 |
| `-ERR increment or decrement would overflow` | INCR/DECR 64 位溢出 |
| `-ERR invalid key` | key 为空字符串 |
| `-ERR invalid score` | ZADD/ZRANGE 的 score 无法解析 |
| `-ERR invalid integer` | ZRANGE/LRANGE 的 start/stop 无法解析为整数 |
| `-ERR invalid expire time` | SETEX 的 seconds ≤ 0 |
| `-ERR no such key` | DEBUG OBJECT 的 key 不存在 |
| `-ERR invalid serialized data for <TYPE>` | RESTORE 的序列化数据非法（含 LIST/HASH/SET/ZSET size、score 解析失败） |
| `-BUSYKEY Target key name already exists` | RESTORE 目标 key 已存在且未带 REPLACE |
| `-ERR BGSAVE already in progress` | BGSAVE 重入（SAVE 亦受此限制） |
| `-ERR DEBUG SLEEP is not supported` | DEBUG SLEEP 已移除 |
| `-WRONGTYPE Operation against a key holding the wrong kind of value` | 对非预期类型的 key 执行类型敏感命令 |
| `-ERR Protocol error: <原因>`（随后断开连接） | RESP 结构非法，见 §17 |

## 16. 不支持的 Redis 特性

> 与原生 Redis 相比，当前版本**不提供**：

- 鉴权（`AUTH` / `ACL`）
- TLS 加密连接
- 事务（`MULTI` / `EXEC` / `WATCH`）
- 脚本（`EVAL` / Lua）
- 发布订阅（`PUBLISH` / `SUBSCRIBE`）
- Stream 数据类型
- Bitmap / HyperLogLog / Geo 数据类型
- 模块系统（`MODULE LOAD`）
- 慢日志（`SLOWLOG`）
- 客户端列表（`CLIENT LIST`）
- 键扫描（`SCAN` / `KEYS`）
- HSET 多 field/value 对（仅支持单对）
- ZRANGE 的 BYSCORE/BYLEX/REV/LIMIT 选项

## 17. 协议格式（RESP 2.0）

```text
+OK\r\n                      简单字符串
-ERR ...\r\n                 错误
-WRONGTYPE ...\r\n           类型错误
:123\r\n                     整数
$5\r\nhello\r\n              批量字符串
$-1\r\n                      Nil
*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n  数组（命令）
```

编码/解码实现位于 `src/protocol/resp.h`（`RespParser` / `RespEncoder`）。

### 17.1 协议错误（断开连接）

以下**结构非法**的输入会收到 `-ERR Protocol error: <原因>` 并被服务器**立即断开**（与 Redis 行为一致，防止坏字节滞留解析器卡死连接）：

| 输入 | 错误原因 |
|------|---------|
| 首字节不是 `+` `-` `:` `$` `*` | `unknown type byte` |
| `$`/`*` 头部的长度行不是数字 | `invalid number in protocol header` |
| 负长度不是 `-1` | `invalid negative length` |
| Bulk string 长度 > 512MB | `bulk string length exceeds limit` |
| 数组长度 > 100 万元素 | `array length exceeds limit` |

> 注意：**不支持 inline 命令**——直接发送裸文本（如 `PING\r\n`）首字节为 `P`，会被按协议错误拒绝并断开。必须使用 RESP 数组格式。
>
> 与之相对，**单条命令格式错**（parse 级错误，如参数不完整）是可恢复的：服务器回 `-ERR ...` 并重置解析器继续服务，不断开连接。

## 18. 客户端示例

### redis-cli

```bash
redis-cli -p 16379

> SET user:1 "Alice"
OK
> DEL key1 key2 key3
(integer) 2
> HSET user:1 age 25
(integer) 1
> HGETALL user:1
1) "age"
2) "25"
> ZADD leaderboard 100 Alice 200 Bob 300 Charlie
(integer) 3
> ZRANGE leaderboard 0 -1 WITHSCORES
1) "Alice"
2) "100"
3) "Bob"
4) "200"
5) "Charlie"
6) "300"
```

### redis-py

```python
import redis
r = redis.Redis(host='127.0.0.1', port=16379, decode_responses=True)

# 字符串
r.set('counter', 0)
r.incr('counter')   # 1

# 批量删除
r.delete('key1', 'key2', 'key3')  # 返回删除数

# 哈希
r.hset('user:1', 'name', 'Alice')
r.hgetall('user:1')  # {'name': 'Alice'}

# ZSet
r.zadd('lb', {'Alice': 100, 'Bob': 200})
r.zrange('lb', 0, -1, withscores=True)
# [('Alice', 100.0), ('Bob', 200.0)]

# 过期
r.setex('session:abc', 60, 'token-xyz')
```

## 19. 另见

- [架构总览 § 1.1 核心特性](architecture/overview.md)
- [架构总览 § 5 请求处理时序](architecture/overview.md)
- [集群架构 § 8 客户端重定向](architecture/cluster.md)
- [部署文档 § 端口与连接](../deployment.md)
