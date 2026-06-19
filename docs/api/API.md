# ConcurrentCache API 参考文档

> **文档版本**：v3.2 · 适用服务端版本 V3.0+
> **协议**：TCP + Redis RESP 2.0
> **默认端口**：`6379`（可在 `conf/concurrentcache.conf` 修改）
> **客户端兼容**：任意 Redis 客户端（`redis-cli`、`jedis`、`redis-py`、`go-redis`、`ioredis` 等）
> **文档最后更新**：与 `src/command/command_factory.cpp` 注册表保持一致

---

## 📑 目录

- [1. 概览](#1-概览)
- [2. 快速开始](#2-快速开始)
- [3. 核心概念](#3-核心概念)
- [4. 连接与协议](#4-连接与协议)
- [5. API 参考](#5-api-参考)
  - 5.1 [连接](#51-连接)
  - 5.2 [字符串 String](#52-字符串-string)
  - 5.3 [过期 TTL](#53-过期-ttl)
  - 5.4 [列表 List](#54-列表-list)
  - 5.5 [哈希 Hash](#55-哈希-hash)
  - 5.6 [集合 Set](#56-集合-set)
  - 5.7 [有序集合 ZSet](#57-有序集合-zset)
  - 5.8 [持久化](#58-持久化)
  - 5.9 [服务器](#59-服务器)
  - 5.10 [集群](#510-集群)
  - 5.11 [复制](#511-复制)
  - 5.12 [迁移](#512-迁移)
- [6. 错误参考](#6-错误参考)
- [7. 配额与限制](#7-配额与限制)
- [8. 版本变更](#8-版本变更)
- [9. 另见](#9-另见)

---

## 1. 概览

ConcurrentCache 是一套兼容 Redis RESP 协议的内存缓存服务，**支持任意 Redis 客户端直连**，无需专用 SDK。

### 1.1 核心特性

| 特性 | 说明 |
|------|------|
| 数据类型 | String / List / Hash / Set / ZSet 五种 |
| 网络 | MainSubReactor 多线程，epoll LT 模式 |
| 并发 | 64 分片分段锁哈希表 |
| 内存 | 三层内存池（Thread → Central → Page） |
| 持久化 | RDB 快照，启动自动恢复 |
| 集群 | 哈希槽分片 + Gossip 协议 + 主从复制（V4.0+） |

### 1.2 不支持的特性

> 与 Redis 原生版对比，**当前版本不提供**以下能力，请勿依赖：

- ❌ 鉴权（AUTH / ACL）
- ❌ TLS 加密连接
- ❌ 事务（MULTI / EXEC）
- ❌ 脚本（EVAL / Lua）
- ❌ 发布订阅（PUB / SUB）
- ❌ Stream 数据类型
- ❌ 模块系统（MODULE LOAD）

---

## 2. 快速开始

### 2.1 前置条件

- 服务端已启动并监听 `0.0.0.0:6379`
- 客户端机器能访问服务端 6379 端口
- 任意 Redis 客户端工具

### 2.2 验证连接

```bash
$ redis-cli -h 127.0.0.1 -p 6379 PING
PONG
```

看到 `PONG` 即代表连接成功。

### 2.3 第一次读写

#### redis-cli

```bash
$ redis-cli -p 6379
127.0.0.1:6379> SET greeting "Hello, ConcurrentCache"
OK
127.0.0.1:6379> GET greeting
"Hello, ConcurrentCache"
127.0.0.1:6379> DEL greeting
(integer) 1
127.0.0.1:6379> GET greeting
(nil)
```

#### Python（redis-py）

```python
import redis

r = redis.Redis(host='127.0.0.1', port=6379, decode_responses=True)

# 写入
r.set('greeting', 'Hello, ConcurrentCache')

# 读取
print(r.get('greeting'))          # Hello, ConcurrentCache

# 删除
deleted = r.delete('greeting')    # 1
print(r.get('greeting'))          # None
```

#### Java（jedis）

```java
import redis.clients.jedis.Jedis;

try (Jedis jedis = new Jedis("127.0.0.1", 6379)) {
    jedis.set("greeting", "Hello, ConcurrentCache");
    System.out.println(jedis.get("greeting"));   // Hello, ConcurrentCache
    System.out.println(jedis.del("greeting"));   // 1
}
```

#### Go（go-redis v9）

```go
import (
    "context"
    "fmt"
    "github.com/redis/go-redis/v9"
)

func main() {
    rdb := redis.NewClient(&redis.Options{Addr: "127.0.0.1:6379"})
    ctx := context.Background()

    rdb.Set(ctx, "greeting", "Hello, ConcurrentCache", 0)
    val, _ := rdb.Get(ctx, "greeting").Result()
    fmt.Println(val)                          // Hello, ConcurrentCache
    rdb.Del(ctx, "greeting")
}
```

#### Node.js（ioredis）

```javascript
const Redis = require('ioredis');
const r = new Redis({ host: '127.0.0.1', port: 6379 });

(async () => {
  await r.set('greeting', 'Hello, ConcurrentCache');
  console.log(await r.get('greeting'));      // Hello, ConcurrentCache
  console.log(await r.del('greeting'));      // 1
  r.disconnect();
})();
```

---

## 3. 核心概念

### 3.1 Key 与 Value

- **Key**：任意非空字符串，建议使用 `业务:实体:ID` 形式（如 `user:1001`、`session:abc123`）
- **Value**：根据数据类型不同，可为字符串、列表、哈希、集合、有序集合

### 3.2 数据类型对照

| 类型 | 用途 | 典型场景 |
|------|------|---------|
| String | 普通字符串/数字/JSON | 缓存、计数器、分布式锁 |
| List | 有序可重复序列 | 消息队列、最新列表 |
| Hash | 字段-值映射 | 对象属性、用户资料 |
| Set | 无序不重复集合 | 标签、共同好友 |
| ZSet | 带分数的有序集合 | 排行榜、延迟队列 |

### 3.3 过期机制

- **惰性删除**：访问时检查是否过期
- **定期清理**：后台线程抽样删除过期 key（每 100ms 一轮）
- **设置方式**：`EXPIRE`（秒级）、`SETEX`（设置时同时指定）、`PERSIST`（移除）

### 3.4 持久化机制

- **RDB 快照**：全量数据二进制快照
- **触发方式**：手动 `SAVE`（同步）/ `BGSAVE`（后台异步）
- **恢复方式**：服务启动时自动加载 `dump.rdb`
- **保存路径**：`./dump.rdb`（默认，可在配置修改）

---

## 4. 连接与协议

### 4.1 连接信息

| 配置项 | 默认值 | 说明 |
|--------|-------|------|
| 监听地址 | `0.0.0.0` | 监听所有网卡 |
| 端口 | `6379` | Redis 标准端口 |
| 最大连接数 | 受 `ulimit` 限制 | 无应用层硬限制 |
| 鉴权 | **无** | 当前版本不提供 |

### 4.2 RESP 协议简介

请求和响应均为纯文本，每条以 `\r\n` 结尾。

**请求示例**（`SET name alice`）：

```
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n
```

**响应示例**：

```
+OK\r\n
```

**类型前缀**：

| 前缀 | 含义 |
|------|------|
| `+` | 简单字符串 |
| `-` | 错误 |
| `:` | 整数 |
| `$` | 批量字符串（含 nil 用 `$-1`） |
| `*` | 数组 |

### 4.3 错误处理

- 命令级错误：返回 `-ERR xxx`，**不关闭连接**，客户端可继续发命令
- 协议解析错误：服务端会关闭连接
- 网络断开：客户端需重连

---

## 5. API 参考

> **约定**：每条命令按统一结构说明：语法 → 参数 → 返回值 → 示例（多语言） → 错误场景 → 关联命令

---

### 5.1 连接

#### PING

测试连接是否存活。

| 项 | 说明 |
|---|---|
| **语法** | `PING [message]` |
| **参数** | `message`（可选）— 自定义回显文本 |
| **返回值** | 无参：`PONG`；有参：回显 `message` |
| **复杂度** | O(1) |

**示例**：

```bash
> PING
PONG

> PING hello
"hello"
```

```python
print(r.ping())           # True
print(r.ping('hello'))    # b'hello'
```

**关联命令**：无

---

### 5.2 字符串 String

#### GET

获取 key 的字符串值。

| 项 | 说明 |
|---|---|
| **语法** | `GET key` |
| **参数** | `key` — 要查询的键 |
| **返回值** | 字符串值（Bulk String）/ `nil`（key 不存在）/ 错误（类型不匹配） |
| **复杂度** | O(1) |

**示例**：

```bash
> SET name alice
OK
> GET name
"alice"
> GET notexist
(nil)
```

```python
r.set('name', 'alice')
r.get('name')           # 'alice'
r.get('notexist')       # None
```

**错误场景**：

- key 存在但类型不是 String → `ERR not a string`

**关联命令**：[SET](#set) · [DEL](#del) · [EXISTS](#exists)

---

#### SET

设置 key 的字符串值。

| 项 | 说明 |
|---|---|
| **语法** | `SET key value` |
| **参数** | `key` — 键名；`value` — 字符串值（任意字节） |
| **返回值** | `OK` |
| **复杂度** | O(1) |
| **副作用** | 覆盖已有值，重置过期时间（如果之前有） |

**示例**：

```bash
> SET name alice
OK
> SET name bob      # 覆盖
OK
> GET name
"bob"
```

```java
jedis.set("name", "alice");
```

**错误场景**：

- 参数数量错误 → `ERR wrong number of arguments for 'set' command`

**关联命令**：[GET](#get) · [SETEX](#setex) · [DEL](#del)

---

#### SETEX

设置值并指定过期时间（秒），原子操作。

| 项 | 说明 |
|---|---|
| **语法** | `SETEX key seconds value` |
| **参数** | `key` — 键名；`seconds` — 过期秒数（> 0）；`value` — 字符串值 |
| **返回值** | `OK` |
| **复杂度** | O(1) |

**示例**：

```bash
> SETEX session 60 abc123
OK
> TTL session
(integer) 60
```

```python
r.setex('session', 60, 'abc123')
```

**错误场景**：

- `seconds ≤ 0` → `ERR invalid expire time`
- `seconds` 非数字 → `ERR value is not an integer`

**关联命令**：[SET](#set) · [EXPIRE](#expire) · [TTL](#ttl)

---

#### DEL

删除一个 key。

| 项 | 说明 |
|---|---|
| **语法** | `DEL key` |
| **参数** | `key` — 要删除的键 |
| **返回值** | `1`（删除成功）/ `0`（key 不存在） |
| **复杂度** | O(1) |

**示例**：

```bash
> SET temp 1
OK
> DEL temp
(integer) 1
> DEL temp
(integer) 0
```

**关联命令**：[EXISTS](#exists) · [FLUSHDB](#flushdb)

---

#### INCR

将 key 的整数值原子加 1。key 不存在时视为 0。

| 项 | 说明 |
|---|---|
| **语法** | `INCR key` |
| **参数** | `key` — 键名（值必须可解析为整数） |
| **返回值** | 递增后的整数值 |

**示例**：

```bash
> SET counter 10
OK
> INCR counter
(integer) 11
> INCR counter
(integer) 12
> INCR notexist   # 不存在视为 0，加 1 后是 1
(integer) 1
```

**错误场景**：

- 原值不是合法整数 → `ERR value is not an integer`

**关联命令**：[DECR](#decr) · [GET](#get) · [SET](#set)

---

#### DECR

将 key 的整数值原子减 1。语义同 INCR。

| 项 | 说明 |
|---|---|
| **语法** | `DECR key` |
| **返回值** | 递减后的整数值 |

---

#### EXISTS

判断 key 是否存在。

| 项 | 说明 |
|---|---|
| **语法** | `EXISTS key` |
| **返回值** | `1`（存在）/ `0`（不存在） |

**关联命令**：[DEL](#del)

---

### 5.3 过期 TTL

#### EXPIRE

设置 key 的过期时间。覆盖已有过期时间。

| 项 | 说明 |
|---|---|
| **语法** | `EXPIRE key seconds` |
| **参数** | `seconds` — 过期秒数（必须 > 0） |
| **返回值** | `1`（设置成功）/ `0`（key 不存在） |

**示例**：

```bash
> SET temp 1
OK
> EXPIRE temp 300
(integer) 1
> TTL temp
(integer) 300
```

**错误场景**：

- `seconds ≤ 0` → 返回 `0`（不报错，但不会设置过期）
- `seconds` 非数字 → `ERR value is not an integer`

**关联命令**：[TTL](#ttl) · [PTTL](#pttl) · [PERSIST](#persist) · [SETEX](#setex)

---

#### TTL

查看 key 剩余生存时间（秒）。

| 项 | 说明 |
|---|---|
| **语法** | `TTL key` |
| **返回值** | `正整数`（剩余秒数）<br>`-1`（key 存在但无过期时间）<br>`-2`（key 不存在） |

**示例**：

```bash
> SET a 1
OK
> TTL a
(integer) -1
> EXPIRE a 100
(integer) 1
> TTL a
(integer) 100
```

**关联命令**：[PTTL](#pttl) · [EXPIRE](#expire)

---

#### PTTL

同 TTL，返回毫秒精度。

#### PERSIST

移除 key 的过期时间，让其永不过期。

| 项 | 说明 |
|---|---|
| **语法** | `PERSIST key` |
| **返回值** | `1`（成功移除）/ `0`（key 不存在或原本就无过期） |

**关联命令**：[EXPIRE](#expire) · [TTL](#ttl)

---

### 5.4 列表 List

#### LPUSH

从列表**左侧**推入一个或多个元素。

| 项 | 说明 |
|---|---|
| **语法** | `LPUSH key value [value ...]` |
| **返回值** | 推入后列表的长度 |

**示例**：

```bash
> LPUSH tasks job1
(integer) 1
> LPUSH tasks job2 job3
(integer) 3
> LRANGE tasks 0 -1
1) "job3"
2) "job2"
3) "job1"
```

**关联命令**：[RPUSH](#rpush) · [LPOP](#lpop) · [LRANGE](#lrange)

---

#### RPUSH

从列表**右侧**推入一个或多个元素。语义同 LPUSH。

#### LPOP

从列表左侧弹出一个元素。

| **返回值** | 弹出的元素 / `nil`（key 不存在或列表为空） |

#### RPOP

从列表右侧弹出一个元素。语义同 LPOP。

#### LLEN

获取列表长度。key 不存在时返回 `0`。

#### LRANGE

获取列表指定范围内的元素。

| 项 | 说明 |
|---|---|
| **语法** | `LRANGE key start stop` |
| **参数** | `start` / `stop` — 索引；支持负数（`-1` 表示最后一个元素） |
| **返回值** | 元素数组（含端点） |

**示例**：

```bash
> RPUSH nums 1 2 3 4 5
(integer) 5
> LRANGE nums 0 2       # 前 3 个
1) "1"
2) "2"
3) "3"
> LRANGE nums 0 -1      # 全部
1) "1"
2) "2"
3) "3"
4) "4"
5) "5"
```

**关联命令**：[LPUSH](#lpush) · [LLEN](#llen)

---

### 5.5 哈希 Hash

#### HSET

设置一个或多个字段。

| 项 | 说明 |
|---|---|
| **语法** | `HSET key field value` |
| **返回值** | `1`（新增字段）/ `0`（覆盖已有字段） |

**示例**：

```bash
> HSET user:1 name alice
(integer) 1
> HSET user:1 age 25
(integer) 1
> HSET user:1 name alice2    # 覆盖
(integer) 0
```

**关联命令**：[HGET](#hget) · [HGETALL](#hgetall) · [HDEL](#hdel)

---

#### HGET

获取字段值。字段不存在时返回 `nil`。

#### HDEL

删除一个或多个字段。返回**实际删除数量**。

| **语法** | `HDEL key field [field ...]` |

#### HLEN

获取字段数量。key 不存在时返回 `0`。

#### HGETALL

获取全部字段与值。

| **返回值** | 数组中 field 与 value 交替出现 |

**示例**：

```bash
> HGETALL user:1
1) "name"
2) "alice2"
3) "age"
4) "25"
```

**关联命令**：[HSET](#hset) · [HGET](#hget)

---

### 5.6 集合 Set

#### SADD

添加一个或多个成员。返回**实际新增数量**（已存在的不算）。

| **语法** | `SADD key member [member ...]` |

#### SPOP

随机弹出一个成员。返回弹出的成员 / `nil`（空集或 key 不存在）。

> 内部使用 `std::mt19937` 线程局部随机数生成器。

#### SCARD

获取成员数量。key 不存在时返回 `0`。

#### SISMEMBER

判断 member 是否在集合中。返回 `1`（在）/ `0`（不在）。

#### SMEMBERS

获取全部成员。返回成员数组。

**示例**：

```bash
> SADD tags redis cache nosql
(integer) 3
> SISMEMBER tags redis
(integer) 1
> SPOP tags
"cache"
> SMEMBERS tags
1) "redis"
2) "nosql"
```

**关联命令**：[SADD](#sadd) · [SCARD](#scard) · [SISMEMBER](#sismember)

---

### 5.7 有序集合 ZSet

#### ZADD

添加一个成员及分数。返回 `1`（新增）/ `0`（更新分数）。

| **语法** | `ZADD key score member` |

**示例**：

```bash
> ZADD rank 100 alice
(integer) 1
> ZADD rank 200 bob
(integer) 1
> ZADD rank 150 charlie
(integer) 1
```

#### ZSCORE

获取成员分数。返回分数字符串 / `nil`（成员不存在）。

#### ZCARD

获取成员数量。key 不存在时返回 `0`。

#### ZRANGE

按**分数范围**获取成员。

| 项 | 说明 |
|---|---|
| **语法** | `ZRANGE key min max [WITHSCORES]` |
| **参数** | `min` / `max` — 分数范围（闭区间）<br>`WITHSCORES` — 可选，带上则数组中 member 与 score 交替出现 |

**示例**：

```bash
> ZRANGE rank 0 200
1) "alice"
2) "charlie"
3) "bob"
> ZRANGE rank 0 200 WITHSCORES
1) "alice"
2) "100"
3) "charlie"
4) "150"
5) "bob"
6) "200"
```

**关联命令**：[ZADD](#zadd) · [ZSCORE](#zscore) · [ZCARD](#zcard)

---

### 5.8 持久化

#### SAVE

**同步**保存 RDB 快照。执行期间阻塞所有客户端命令。

| **语法** | `SAVE` |
| **返回值** | `OK` / `ERR failed to save RDB` |

#### BGSAVE

**后台异步**保存快照，立即返回。

| **返回值** | `Background saving started` |

> `SAVE` 与 `BGSAVE` **互斥**，同时只能有一个持久化操作；并发请求后者返回 `ERR BGSAVE already in progress`。

#### LASTSAVE

返回上次成功 BGSAVE 的 Unix 时间戳（秒）。未执行过返回 `0`。

**关联命令**：[INFO persistence](#info) · [FLUSHDB](#flushdb)

---

### 5.9 服务器

#### DBSIZE

返回当前数据库的 key 数量。

#### FLUSHDB

清空当前数据库全部 key。**不可恢复，慎用**。

#### INFO

返回服务器信息与统计。

| **语法** | `INFO [section]` |
| **section** | `server` / `stats` / `persistence` / `keyspace` / `all`（默认） |

**示例**：

```bash
> INFO server
# Server
concurrentcache_version:3.0.0
os:Linux
arch_bits:64
```

**关联命令**：[DEBUG](#debug) · [DBSIZE](#dbsize)

---

#### DEBUG

调试用，支持两个子命令：

| 子命令 | 作用 | 示例 |
|--------|------|------|
| `DEBUG sleep <seconds>` | 让服务端休眠指定秒数 | `DEBUG sleep 1.5` |
| `DEBUG object <key>` | 返回 key 的数据类型 | `DEBUG object user:1` → `Type: hash` |

**示例**：

```bash
> DEBUG object user:1
"Type: hash"
> DEBUG sleep 0.5
OK
```

---

### 5.10 集群

#### CLUSTER

集群管理入口命令，附带多个子命令，与 Redis Cluster 兼容。

| 子命令 | 作用 |
|--------|------|
| `CLUSTER INFO` | 返回集群状态信息 |
| `CLUSTER NODES` | 返回集群节点列表 |
| `CLUSTER MEET <ip> <port>` | 加入新节点 |
| `CLUSTER ADDSLOTS <slot> [slot ...]` | 分配哈希槽 |
| 其他 | 见 `src/cluster/cluster_cmd.cpp` |

> 单端口仅本机模式时，大部分子命令不可用。集群模式需启用 `cluster_enabled = true`。

---

### 5.11 复制

| 命令 | 作用 |
|------|------|
| `PSYNC <replid> <offset>` | 复制握手（Redis 2.8+ 协议） |
| `SYNC` | 兼容旧版复制协议 |
| `REPLCONF <key> <value>` | 主从间配置交换（端口、IP、ACK 等） |

> 这三个命令是**主从复制内部协议**，普通客户端无需直接调用。

---

### 5.12 迁移

#### RESTORE

反序列化一个 RDB 编码的 value 并写入指定 key，常与 MIGRATE 配合使用。

| **语法** | `RESTORE key ttl serialized-value [REPLACE]` |
| **参数** | `ttl` — 过期时间（毫秒），0 表示永不过期；`REPLACE` — 覆盖已有 key |

---

## 6. 错误参考

ConcurrentCache 不使用数字错误码，所有错误以 `ERR xxx` 文本形式返回。

### 6.1 通用错误

| 错误信息 | 触发场景 |
|---------|---------|
| `ERR wrong number of arguments for '<cmd>' command` | 参数数量不对 |
| `ERR invalid key` | key 为空字符串 |
| `ERR value is not an integer` | 期望整数的位置给了非数字 |
| `ERR invalid integer` | LRANGE 索引非法 |
| `ERR invalid score` | ZADD/ZRANGE 分数非法 |
| `ERR invalid expire time` | SETEX/EXPIRE 的 seconds ≤ 0 |
| `ERR not a string` | 对非字符串类型执行 GET |

### 6.2 持久化错误

| 错误信息 | 触发场景 |
|---------|---------|
| `ERR BGSAVE already in progress` | SAVE/BGSAVE 互斥冲突 |
| `ERR failed to save RDB` | 持久化写盘失败 |
| `ERR bgsave failed` | 后台保存启动失败 |

### 6.3 服务器错误

| 错误信息 | 触发场景 |
|---------|---------|
| `ERR Unknown INFO section` | INFO 段名不识别 |
| `ERR Unknown DEBUG subcommand` | DEBUG 子命令不识别 |
| `ERR no such key` | DEBUG OBJECT 时 key 不存在 |

---

## 7. 配额与限制

| 项 | 当前限制 | 备注 |
|----|---------|------|
| Key 最大长度 | 理论无限制 | 实际受单 value 内存限制 |
| Value 最大长度 | 理论无限制 | 实际受可用内存限制 |
| 单 key 元素数（List/Hash/Set/ZSet） | 理论无限制 | 实际受内存限制 |
| 同时连接数 | 受 OS `ulimit` 限制 | 无应用层硬限制 |
| 请求体大小 | 理论无限制 | 实际受 socket 缓冲区限制 |
| 数据库数量 | 1（`db0`） | 当前版本不支持多库切换 |

---

## 8. 版本变更

### v3.0（当前）

- 完整支持 5 种数据类型（String/List/Hash/Set/ZSet）
- RDB 持久化（SAVE / BGSAVE / LASTSAVE）
- 服务器命令（DBSIZE / FLUSHDB / INFO / DEBUG）
- 过期管理（EXPIRE / TTL / PTTL / PERSIST / SETEX）
- 计数器命令（INCR / DECR）
- 内存池（ThreadCache + CentralCache + PageCache）
- MainSubReactor 多线程网络模型

### v2.0

- MainSubReactor 架构
- 内存池基础
- 锁机制

### v1.0

- 单 Reactor 骨架
- 基础命令（PING / GET / SET / DEL）

### v4.0（计划中）

- 集群模式（哈希槽分片）
- Gossip 协议
- 主从复制

---

## 9. 另见

### 9.1 相关文档

- [项目 README](../../README.md) — 编译、运行、测试快速上手
- [架构总览](../developing/Architecture.md) — 系统架构演进
- [网络层设计](../developing/CurrentProject_Detailed/05_网络层.md) — MainSubReactor 详解
- [协议层设计](../developing/CurrentProject_Detailed/03_协议层.md) — RESP 协议解析
- [存储层设计](../developing/CurrentProject_Detailed/04_存储层.md) — 64 分片与 ExpireDict

### 9.2 外部参考

- [Redis 官方文档](https://redis.io/documentation) — RESP 协议与命令规范
- [Redis 设计与实现](https://github.com/huangz1990/redisbook) — 实现原理
- [muduo 网络库](https://github.com/chenshuo/muduo) — Reactor 模式参考

### 9.3 源码索引

- 命令注册表：`src/command/command_factory.cpp`
- 字符串/List/Hash/Set/ZSet 实现：`src/command/string_cmd.h`
- 过期命令实现：`src/command/expire_cmd.h`
- 持久化与服务器命令：`src/command/string_cmd.h`（Save/Bgsave/Info/Debug）
- 集群命令：`src/command/cluster_cmd.cpp`
- 复制命令：`src/command/psync_cmd.cpp`
- RESP 协议编码：`src/protocol/resp.cpp`

---

**文档维护说明**：本接口列表与 `src/command/command_factory.cpp` 的 `register_command` 调用保持同步。新增/修改命令时，请同步更新本文件对应章节。
