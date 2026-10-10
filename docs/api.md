# ConcurrentCache API 参考

> **协议**：Redis RESP 2.0（兼容任意 Redis 客户端：`redis-cli`、`jedis`、`redis-py`、`go-redis`）
> **默认端口**：`6379`（`conf/concurrentcache.conf` 中 `port` 项可改；集群总线端口 = 客户端端口 + 10000，默认即 `16379`）
> **命令总数**：46 个（注册于 `src/command/command_factory.cpp`）
> **文档维护**：与命令注册表严格同步，修改注册表必须同步本文档

## 1. 快速开始

```bash
$ redis-cli -p 6379 PING
PONG
```

```python
# Python (redis-py)
import redis
r = redis.Redis(host='127.0.0.1', port=6379, decode_responses=True)
r.set('greeting', 'Hello, ConcurrentCache')
print(r.get('greeting'))   # Hello, ConcurrentCache
```

## 2. 命令索引

| 分类 | 命令数 | 命令 |
|------|--------|------|
| [§ 3 连接](#3-连接) | 1 | PING |
| [§ 4 字符串](#4-字符串-string) | 8 | GET / SET / DEL / EXISTS / INCR / DECR / INCRBY / DECRBY |
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
| `SET` | `SET key value [EX s \| PX ms \| EXAT unix秒 \| PXAT unix毫秒 \| KEEPTTL] [NX \| XX] [GET]` | 无选项时 `+OK\r\n`（覆盖写入并清除该 key 已有的 TTL，等同 PERSIST）；`NX`/`XX` 条件不满足回 `$-1\r\n`；带 `GET` 回写入前的旧值（旧值不是字符串类型时回 `WRONGTYPE`）；`EX`/`PX` 的值 ≤ 0，或 `EX` 的秒数大到换算成毫秒会溢出时返回 `-ERR invalid expire time in 'set' command`（`EXAT`/`PXAT` 只在 < 0 时拒） | O(1) |
| `DEL` | `DEL key [key ...]` | `:N\r\n`（删除成功的 key 数） | O(N) |
| `EXISTS` | `EXISTS key` | `:1\r\n` 或 `:0\r\n`（已过期但尚未被删除的 key 返回 0） | O(1) |
| `INCR` | `INCR key` | 递增后整数值（原子） | O(1) |
| `DECR` | `DECR key` | 递减后整数值（原子） | O(1) |
| `INCRBY` | `INCRBY key delta` | 加 delta 后的整数值（原子） | O(1) |
| `DECRBY` | `DECRBY key delta` | 减 delta 后的整数值（原子） | O(1) |

> **DEL** 支持批量删除多个 key，返回实际删除成功的数量。
> **INCR/DECR/INCRBY/DECRBY** 是**原子操作**（读-改-写在分片独占锁内完成，并发调用不丢失更新）：若 key 不存在视为 0；若 delta 无法解析返回 `-ERR value is not an integer or out of range`；四条命令的 64 位溢出统一返回 `-ERR increment or decrement would overflow`；若 key 现值非整数返回 `-ERR value is not an integer`（现值用 `std::stoll` 读，**只要求前缀是整数**：值为 `12abc` 时 INCR 得到 13，Redis 则会报错）；若 key 持有非 STRING 类型返回 `-WRONGTYPE Operation against a key holding the wrong kind of value`。
> **EXISTS / HSET / SPOP 只接受单个 key（或单个 field/value 对）**，多传参数返回 `-ERR wrong number of arguments for '<cmd>' command`——与 Redis 的多参数形式不同，见 §16。

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
> **LRANGE** 的 start/stop 支持负索引（-1 表示最后一个元素）。stop 换算后仍为负时被**夹到 0**：
> `LRANGE k 0 -3` 在 2 个元素的表上返回下标 0 的元素。Redis 在这种情形返回空列表，本服务器的
> ZRANGE 也返回空 —— 这是 LRANGE 与 Redis 的一处已知差异。
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
> **ZRANGE** 按排名索引（index）范围查询，start/stop 支持负索引（-1 表示最后一个）；stop 换算后仍为负
> 直接返回空列表（与上面 LRANGE 的夹到 0 不同，与 Redis 一致）。可选 `WITHSCORES` 参数同时返回分数。
> **不支持 Redis 6.2+ 的 BYSCORE/BYLEX/REV/LIMIT 选项**。
> 对非 ZSET 类型的 key 执行有序集合命令返回 `-WRONGTYPE` 错误。

## 10. 持久化

| 命令 | 语法 | 返回 |
|------|------|------|
| `SAVE` | `SAVE` | `+OK\r\n` / `-ERR failed to save RDB`（保存路径取 `rdb_path` 配置） |
| `BGSAVE` | `BGSAVE` | `+Background saving started\r\n` / `-ERR bgsave failed` |
| `LASTSAVE` | `LASTSAVE` | 上次成功保存的 Unix 时间戳（秒，整数） |
| `DBSIZE` | `DBSIZE` | 底层哈希表的条目数（整数）。**已过期但尚未被删除的 key 会计入** —— `GlobalStorage::size()` 只累加分片 map 的 size，不做过期过滤；惰性删除只让 GET/EXISTS 看不到它们 |
| `FLUSHDB` | `FLUSHDB` | `+OK\r\n`（清空全部数据） |

> **BGSAVE** 进行中再次触发（含 SAVE）→ `-ERR BGSAVE already in progress`。RDB 保存为进程内后台线程快照，先写 `.tmp` 临时文件再原子 rename（详见[持久化架构](architecture/persistence.md)）。

## 11. 服务器

| 命令 | 语法 | 返回 |
|------|------|------|
| `INFO` | `INFO [section]` | Bulk String（server/stats/memory/persistence/keyspace/all） |
| `DEBUG` | `DEBUG OBJECT <key>`（子命令**区分大小写**，只认小写 `object`/`sleep`，大写回 `-ERR Unknown DEBUG subcommand`） | 类型信息（Bulk String，如 `Type: string`）；key 不存在返回 `-ERR no such key` |

> **DEBUG SLEEP 已被移除**：`DEBUG SLEEP <sec>` 会阻塞事件循环，现返回 `-ERR DEBUG SLEEP is not supported`。
> **`# Memory` 里没有 `used_memory`**：Redis 那一栏是分配器报告的已用字节，而本项目那三层内存池还没接进任何分配路径，没有可信值可报。字段缺失比一个抄来的数字好。这里报的是内核视角的 `used_memory_rss`（读 `/proc/self/status` 的 `VmRSS`），加一个 `used_memory_keys`（条目数，不是字节）。淘汰只看条数，所以 `maxmemory` 恒为 `0`（= 没有按字节的硬上限）、`maxmemory_policy` 说明实际按什么在淘汰。
>
> `total_connections_received`（成功 accept 的连接数）与 `total_commands_processed`（造出命令对象并进入执行路径的条数；`MOVED`/`ASK` 重定向与未知命令名不计入）是进程内实时累计，重启归零。`avg_ttl` 单位是**毫秒**；`expires` / `avg_ttl` 只统计还没过期的条目，而 `keys` 是底层哈希表条目数（含已过期未删除的），所以 `keys=100,expires=3` 是正常的，不是矛盾。
> section 名区分大小写，只认 `server` / `stats` / `persistence` / `memory` / `keyspace` / `all`，其它值返回 `-ERR Unknown INFO section: <name>`。

`INFO` 输出示例：

```text
# Server
concurrentcache_version:4.0.0
os:Linux
arch_bits:64
# Stats
total_connections_received:17
total_commands_processed:4821
total_bgsave_calls:5
total_rdb_saved_keys:12345
# Memory
used_memory_rss:52428800
used_memory_rss_human:50MB
maxmemory:0
maxmemory_human:0B
maxmemory_policy:aru-random-shard-sampling
used_memory_keys:12345
# Persistence
rdb_last_bgsave_status:ok
rdb_last_bgsave_time_sec:1718700000
rdb_dirty_count:0
rdb_last_bgsave_keys:12345
# Keyspace
db0:keys=12345,expires=312,avg_ttl=8412000
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
| `SETSLOT` | `CLUSTER SETSLOT <slot> NODE/MIGRATING/IMPORTING/STABLE` | 设置槽状态；状态词必须**逐字大写**（`cluster_cmd.cpp:546/562/578/602`），MIGRATING/IMPORTING 少写目标节点回的是 `-ERR syntax error, MIGRATING needs target node` 这类带说明的文本，不是裸 `syntax error` |
| `REPLICATE` | `CLUSTER REPLICATE <node-name>` | 将本节点设为某主节点的从节点 |
| `FAIL` | `CLUSTER FAIL` | 强制标记主节点下线 |
| `MIGRATE` | `CLUSTER MIGRATE host port key timeout [REPLACE]` | 键迁移（内部；Redis 顶层 `MIGRATE ... dbid ...` 未注册） |

**重定向响应**：

| 响应 | 含义 |
|------|------|
| `-MOVED <slot> <ip:port>` | 槽已迁出，客户端应永久重定向 |
| `-ASK <slot> <ip:port>` | 槽正在迁入，本次重定向即可 |

## 13. 复制

| 命令 | 语法 | 用途 |
|------|------|------|
| `PSYNC` | `PSYNC <runid> <offset>` / `PSYNC ? -1` | **显式拒绝**：本服务器不接受外部 Redis 副本 |
| `SYNC` | `SYNC` | **显式拒绝**：同上 |
| `REPLCONF` | `REPLCONF <key> <value>` | **显式拒绝**：同上 |

这三条是外部 Redis 副本握手用的。本项目的内部复制**不走客户端口**：`CLUSTER REPLICATE`
通过集群总线向主节点发 `REPLSYNC:<node>`，主节点侧由 `ReplicationMgr::send_rdb_to_replica()`
逐 key 发 `RESTORE key ttl_ms <serialize() 结果> REPLACE`（函数名里的 "rdb" 指的是内存快照，
不是 RDB 文件），之后的写命令也走总线复制。所以 `REPLICAOF <host> <port>` /
`redis-cli --replica` 接一个本项目节点不会被支持。

在支持外部副本之前必须先把 RDB 换成 Redis 的方言（版本字节、类型操作码、EOF + CRC64），
否则"握手成功但一个键都没传"比拒绝更坏。另外 `REPLCONF ACK <n>` 原来会把 `n` 直接写进
本节点的 `master_repl_offset`，而 failover 的新鲜度判据读的就是这个值 —— 该写入路径
已随这三条命令一起移除。

## 14. 迁移

```text
RESTORE <key> <ttl> <serialized-value>
```

- 从 `CLUSTER MIGRATE` 流程接收已序列化的 `CacheObject`
- `ttl` 单位为毫秒；0 表示永不过期
- 配套序列化由 `CacheObject::serialize()` 提供：一行类型标签 + 若干条
  `<字节数>\n<原始字节>` 记录，因此成员/字段里含 `\n`、`\r` 不会再被截断
- 载荷任何一帧不完整都算失败（不再"读到哪算哪"交出半个对象）
- 总线侧同样二进制安全：参数帧自 v2 起是"条数 + 每条 `<字节数>\n<原始字节>`"，
  不再用裸 `\xC0` 拼接，所以含 0xC0 的 value 不会被切断（见
  [集群架构 § 7](architecture/cluster.md)）
- 错误返回：`-ERR invalid TTL`（ttl 非法）/ `-ERR Invalid or malformed serialized payload`
  （反序列化失败）/ `-BUSYKEY Target key name already exists`（key 已存在且未带 REPLACE）

`CLUSTER MIGRATE host port key timeout [REPLACE]`（本项目的签名比 Redis 顶层 `MIGRATE`
少一个 `destination-db` 字段，且 `timeout` 必须在 1..60000 毫秒内）走的是"发到目标 +
等它回复"：只有目标回 `+OK` 之后才删源键（默认语义是移动，不是复制）。超时、连不上、
或目标回了 `-BUSYKEY` 之类的错误时，**源键保持不动**，错误转给客户端。总线的请求/回复用
`CCREQ <id> <RESP 命令>` / `CCRESP <id> <RESP 回复>` 两种标记，帧结构本身没有变。

两条已知限制要写在这：等待发生在命令所在线程，而命令跑在 SubReactor 的事件循环上 ——
等待期间这条 reactor 上的所有连接都停着，所以上面那个 60 秒的硬上限是必需的而不是保守；
另外顶层 `MIGRATE host port key dbid timeout ...`（Redis 的那条）没有注册，只有 `CLUSTER MIGRATE`。

## 15. 错误码

| RESP 响应 | 含义 |
|----------|------|
| `-ERR wrong number of arguments for '<cmd>' command` | 参数个数错误 |
| `-ERR value is not an integer` | 现值不是整数（INCR/DECR/INCRBY/DECRBY 作用在非整数字符串上） |
| `-ERR value is not an integer or out of range` | 整数入参（SET 的 EX/PX/EXAT/PXAT、INCRBY/DECRBY 的 delta）**无法解析**时。溢出走 `increment or decrement would overflow`；SETEX/EXPIRE 的 TTL 解析失败回的是 `-ERR value is not an integer`（`expire_cmd.h:84/316`） |
| `-ERR increment or decrement would overflow` | INCR/DECR/INCRBY/DECRBY **四条**命令的 64 位溢出（`string_cmd.h:282` 是同一处出口） |
| `-ERR invalid key` | **只有** TTL 类命令（EXPIRE/TTL/PTTL/PERSIST/SETEX，见 `expire_cmd.h:45/114/177/244/290`）在空 key 时回这条；SET/GET/DEL/INCR 传空串不会产生它 |
| `-ERR value is not a valid float` | ZADD 的 score 完全读不出来、超出 double 范围，或读完还有尾随字节（`1.5abc`）——`string_cmd.h:1097/1103` |
| `-ERR value is NaN or Infinity` | ZADD 的 score 是 `nan`/`inf` 这类合法浮点字面量但不是有限值（`string_cmd.h:1109`，单独一条文本，与上一行不同） |
| `-ERR syntax error` | SET 的选项组合非法（如 `NX XX`、`EX` 与 `EXAT` 同时出现）。ZRANGE 的第 5 个参数只有逐字等于 `WITHSCORES` 才生效，拼错会被**静默忽略**、返回不带分数的成员数组（`string_cmd.h:1234`），不报错 |
| `-ERR invalid integer` | ZRANGE/LRANGE 的 start/stop 无法解析为整数 |
| `-ERR invalid expire time` | SETEX 的 seconds ≤ 0；SET 的 `EX`/`PX` ≤ 0 回的是更长的 `invalid expire time in 'set' command`（`string_cmd.h:171/175`） |
| `-ERR no such key` | DEBUG OBJECT 的 key 不存在 |
| `-ERR invalid TTL` | RESTORE 的 ttl 不是整数 |
| `-ERR Invalid or malformed serialized payload` | RESTORE 的载荷任一圈（帧）不完整、类型标签不认识、或结尾有多余字节 |
| `-BUSYKEY Target key name already exists` | RESTORE 目标 key 已存在且未带 REPLACE |
| `-ERR BGSAVE already in progress` | BGSAVE 重入（SAVE 亦受此限制） |
| `-ERR this server does not accept external replicas; internal replication uses the cluster bus` | PSYNC / SYNC / REPLCONF（见 §13） |
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
- HSET 多 field/value 对（`HSET key field value`，仅单对）
- EXISTS 多 key（`EXISTS key`，仅单键）
- SPOP 的 count 参数（`SPOP key`）
- ZRANGE 的 BYSCORE/BYLEX/REV/LIMIT 选项
- `HELLO`（因此会话恒为 RESP2；服务端只识别 `+ - : $ *` 五种类型字节，RESP3 的 `% ~ > , ( # =` 会被按 `unknown type byte` 拒绝并断开）
- `SELECT`（只有一个逻辑库，数据全在 db0，切换库无意义）
- `DUMP`（只有 RESTORE，没有反向导出）
- `MSET` / `MGET` / `GETSET` / `GETDEL`
- 顶层 `MIGRATE host port key dbid timeout`（只有 `CLUSTER MIGRATE host port key timeout [REPLACE]`）
- `REPLICAOF` / `SLAVEOF` / `CONFIG` / `SHUTDOWN` / `MONITOR`

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
| 数组长度 > 1,048,576（1024×1024）个元素 | `array length exceeds limit`（`resp.cpp:92`） |

> 注意：**不支持 inline 命令**——直接发送裸文本（如 `PING\r\n`）首字节为 `P`，会被按协议错误拒绝并断开。必须使用 RESP 数组格式。
>
> 与之相对，**单条命令格式错**（parse 级错误，如参数不完整）是可恢复的：服务器回 `-ERR ...` 并重置解析器继续服务，不断开连接。

## 18. 客户端示例

### redis-cli

```bash
redis-cli -p 6379

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
r = redis.Redis(host='127.0.0.1', port=6379, decode_responses=True)

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
- [部署文档 § 端口与连接](deployment.md)
