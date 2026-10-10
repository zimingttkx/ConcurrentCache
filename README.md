# ConcurrentCache

**C++ 高性能内存缓存系统 | Redis RESP 协议兼容**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/zimingttkx/ConcurrentCache/ci.yml?style=flat-square)](https://github.com/zimingttkx/ConcurrentCache/actions)

## 简介

ConcurrentCache 是纯 C++20 实现的内存对象缓存系统，兼容 Redis RESP 协议，支持 `redis-cli` 及任意 Redis 客户端连接操作。

本项目旨在通过从零实现，深入理解 Linux 高性能网络编程、并发控制、缓存系统设计等核心技术。

## 核心特性

| 特性 | 说明 |
|------|------|
| 高性能网络模型 | MainReactor + SubReactorPool 多线程 Reactor 架构，epoll 多路复用 |
| 线程安全存储 | 64 分片分段锁哈希表，降低锁竞争 |
| 高效内存管理 | ThreadCache（无锁）→ CentralCache（细粒度锁）→ PageCache 三层架构（已实现，当前未接入分配路径） |
| 协议兼容 | RESP 2.0 命令解析与回复编码，支持 STRING/LIST/HASH/SET/ZSET 五种数据类型 |
| 持久化 | RDB 快照（进程内后台线程，非 fork/COW），原子落盘 tmp→fsync→rename，服务重启自动恢复 |
| 集群支持 | V4.0 支持哈希槽分片、Gossip 协议、主从复制 |
| 内存池现状 | `ThreadCache`/`CentralCache`/`PageCache` 三层池已实现并有单元测试，但**尚未接入任何分配路径**：`new`/容器仍走 glibc malloc，读它的设计文档请当作待接线模块 |

## 技术规格

| 项目 | 规格 |
|------|------|
| 语言标准 | C++20 |
| 目标平台 | Linux (x86_64) |
| 构建系统 | CMake 3.20+ |
| 网络模型 | MainReactor + SubReactorPool (epoll LT) |
| 依赖库 | ZLIB |
| 协议 | Redis RESP 2.0（服务端只解析 `+ - : $ *` 五种类型字节；未实现 RESP3，也不支持 inline 命令） |

## 架构设计

### 整体架构

```
                    redis-cli / jedis / redis-py
                              │
                              │ TCP (RESP)
                              ▼
                    ┌─────────────────────┐
                    │    MainReactor      │
                    │  (单线程 accept)    │
                    └──────────┬──────────┘
                               │ 轮询分发
                    ┌──────────▼──────────┐
                    │   SubReactorPool    │
                    │ ┌────┐ ┌────┐       │
                    │ │ SR │ │ SR │ ...   │
                    │ │  0 │ │  1 │       │
                    │ └────┘ └────┘       │
                    └─────────────────────┘
```

### 核心模块

| 层级 | 模块 | 职责 |
|------|------|------|
| 网络层 | MainReactor | 端口监听，accept 新连接 |
| | SubReactorPool | 管理 SubReactor 线程，轮询负载均衡 |
| | EventLoop | epoll 事件循环 |
| | Connection | TCP 连接，缓冲区管理 |
| 缓存层 | GlobalStorage | 64 分片哈希表，线程安全 |
| | ExpireDict | 过期键管理 |
| | ExpirationChecker | 后台过期键清理 |
| 内存池 | ThreadCache | 线程本地缓存，无锁分配（**当前未接入分配路径**） |
| | CentralCache | 中心缓存，细粒度锁（同上） |
| | PageCache | 页缓存，与系统交互（同上） |
| 命令层 | CommandFactory | 命令统一管理 |
| 持久化层 | RDB | 快照持久化 |
| | RDBScheduler | 后台线程快照调度（间隔 + 脏键阈值触发） |

## 支持的命令

### STRING

| 命令 | 说明 |
|------|------|
| GET key | 获取值 |
| SET key value [EX s \| PX ms \| EXAT 秒 \| PXAT 毫秒 \| KEEPTTL] [NX \| XX] [GET] | 设置值，可带过期与条件；裸 `SET` 会清掉旧 TTL |
| DEL key [key ...] | 删除键（支持多键） |
| EXISTS key | 检查键是否存在（仅单键） |
| INCR key / DECR key | 原子增减 1 |
| INCRBY key delta / DECRBY key delta | 原子增减 delta |
| PING [message] | 心跳检测 |
| EXPIRE key seconds | 设置过期时间（秒） |
| TTL key | 获取剩余生存时间（秒） |
| PTTL key | 获取剩余生存时间（毫秒） |
| PERSIST key | 移除过期时间 |
| SETEX key seconds value | 设置值并指定过期时间 |

### LIST

| 命令 | 说明 |
|------|------|
| LPUSH key value [value ...] | 左侧推入 |
| RPUSH key value [value ...] | 右侧推入 |
| LPOP key | 左侧弹出 |
| RPOP key | 右侧弹出 |
| LLEN key | 获取长度 |
| LRANGE key start stop | 范围查询（支持负索引） |

### HASH

| 命令 | 说明 |
|------|------|
| HSET key field value | 设置字段（仅单对） |
| HGET key field | 获取字段值 |
| HDEL key field [field ...] | 删除字段 |
| HLEN key | 获取字段数量 |
| HGETALL key | 获取所有字段和值 |

### SET

| 命令 | 说明 |
|------|------|
| SADD key member [member ...] | 添加成员 |
| SPOP key | 随机弹出（无 count 参数） |
| SCARD key | 获取成员数量 |
| SISMEMBER key member | 检查成员是否在集合中 |
| SMEMBERS key | 获取所有成员 |

### ZSET

| 命令 | 说明 |
|------|------|
| ZADD key score member [score member ...] | 添加成员及分数 |
| ZSCORE key member | 获取成员分数 |
| ZCARD key | 获取成员数量 |
| ZRANGE key start stop [WITHSCORES] | 按索引范围查询 |

### RDB

| 命令 | 说明 |
|------|------|
| SAVE | 同步保存快照 |
| BGSAVE | 后台异步保存快照 |
| LASTSAVE | 获取上次保存时间戳 |
| DBSIZE | 底层条目数（含已过期未删除的 key） |
| FLUSHDB | 清空全部数据 |

### 服务器 / 集群 / 复制 / 迁移

| 命令 | 说明 |
|------|------|
| INFO [section] | server / stats / persistence / keyspace / all |
| DEBUG OBJECT key | 查看 key 的类型（`DEBUG SLEEP` 已移除） |
| CLUSTER MEET/NODES/INFO/ADDSLOTS/SLOTS/DELSLOTS/SETSLOT/REPLICATE/FAIL/MIGRATE | 10 个子命令 |
| RESTORE key ttl payload | 装载 `CacheObject::serialize()` 的载荷 |
| PSYNC / SYNC / REPLCONF | 显式拒绝：本服务器不接受外部副本，内部复制走集群总线 |

## 快速开始

### 环境要求

- Linux (x86_64)
- GCC 12+ / Clang 16+
- CMake 3.20+
- ZLIB

### 编译

```bash
git clone https://github.com/zimingttkx/ConcurrentCache.git
cd ConcurrentCache
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

### 运行

```bash
./concurrentcache-server
# 默认监听 0.0.0.0:6379
```

### 测试

```bash
redis-cli -p 6379

127.0.0.1:6379> PING
PONG

127.0.0.1:6379> SET name concurrentcache
OK

127.0.0.1:6379> GET name
"concurrentcache"

127.0.0.1:6379> HSET user:1 name Alice
(integer) 1

127.0.0.1:6379> HSET user:1 age 25
(integer) 1

127.0.0.1:6379> HGETALL user:1
1) "name"
2) "Alice"
3) "age"
4) "25"

127.0.0.1:6379> ZADD leaderboard 100 Alice 200 Bob 150 Charlie
(integer) 3

127.0.0.1:6379> ZRANGE leaderboard 0 -1 WITHSCORES
1) "Alice"
2) "100"
3) "Charlie"
4) "150"
5) "Bob"
6) "200"
```

## 性能表现

本项目经过严格的压力测试，展现了卓越的高并发处理能力和极低的延迟。以下是性能表现：

### 吞吐量与性能基准
![Benchmark Dashboard](assets/benchmark/benchmark_dashboard.png)

### 延迟分析
![Latency Analysis](assets/benchmark/latency_analysis.png)

### 线性扩展性
![Scalability](assets/benchmark/scalability.png)

## Docker

本仓库的 CI 只做 `docker build` 冒烟验证（`ci.yml` 的 `docker-build`），**没有发布镜像的工作流**，因此不存在可拉取的预构建镜像；请本地构建。

```bash
docker build -t concurrentcache:latest .
docker run -d -p 6379:6379 -v "$PWD/data:/app/data" --name concurrentcache concurrentcache:latest
redis-cli -p 6379 PING
```

### Docker Compose

```yaml
services:
  concurrentcache:
    build: .
    image: concurrentcache:local
    ports:
      - "6379:6379"
    volumes:
      - ./data:/app/data
    restart: unless-stopped
```

```bash
docker-compose up -d
```

## 配置

编辑 `conf/concurrentcache.conf`：

```ini
port = 6379
reactor_count = 32
thread_pool_size = 32
log_level = 3
rdb_path = ./dump.rdb
rdb_save_interval = 3600
rdb_dirty_threshold = 10000
max_entries = 2000000
cluster_enabled = false
```

## 项目结构

```
main.cpp                       # 启动入口（读 conf、起 reactor、载 RDB、优雅退出）
src/
├── base/                      # 基础组件
│   ├── log.cpp/h             # 日志系统
│   ├── config.cpp/h          # 配置管理
│   ├── signal.cpp/h          # 信号处理
│   ├── format.cpp/h          # 字符串格式化
│   ├── lock.cpp/h            # 锁机制
│   └── thread_pool.cpp/h     # 线程池
│
├── network/                   # 网络层
│   ├── socket.cpp/h          # Socket 封装
│   ├── event_loop.cpp/h      # epoll 事件循环
│   ├── channel.cpp/h         # 事件通道
│   ├── connection.cpp/h       # 连接管理
│   ├── buffer.cpp/h          # 缓冲区
│   ├── main_reactor.cpp/h    # MainReactor
│   ├── sub_reactor.cpp/h     # SubReactor
│   └── sub_reactor_pool.cpp/h # SubReactor 池
│
├── memorypool/                # 内存池
│   ├── size_class.cpp/h      # Size Class 计算
│   ├── thread_cache.cpp/h    # 线程本地缓存
│   ├── central_cache.cpp/h   # 中心缓存
│   ├── page_cache.cpp/h      # 页缓存
│   ├── span.cpp/h            # Span 管理
│   └── free_list.cpp/h       # 空闲链表
│
├── protocol/                   # 协议层
│   └── resp.cpp/h            # Redis RESP 协议
│
├── datatype/                  # 数据类型
│   └── object.cpp/h          # CacheObject
│
├── command/                   # 命令层
│   ├── command.h             # 命令基类
│   ├── command_factory.cpp/h  # 命令工厂
│   ├── string_cmd.h          # 连接/字符串/列表/哈希/集合/有序集合/持久化/服务器命令
│   ├── expire_cmd.h          # EXPIRE / TTL / PTTL / PERSIST / SETEX
│   ├── cluster_cmd.cpp/h     # 集群命令
│   ├── psync_cmd.cpp/h       # 主从同步
│   └── restore_cmd.cpp/h     # 恢复命令
│
├── cache/                     # 缓存核心
│   ├── storage.cpp/h         # GlobalStorage
│   ├── expire_dict.cpp/h      # 过期字典
│   └── expiration_checker.cpp/h # 过期检查
│
├── persistence/               # 持久化
│   ├── rdb.cpp/h            # RDB 格式
│   └── rdb_scheduler.cpp/h   # 快照调度
│
└── cluster/                   # 集群
    ├── cluster_common.h      # 公共定义
    ├── cluster_node.cpp/h    # 节点结构
    ├── cluster_state.cpp/h   # 集群状态
    ├── cluster_server.cpp/h  # 集群服务
    ├── cluster_link.cpp/h   # 节点链接
    ├── cluster_connection.cpp/h # 连接管理
    ├── cluster_gossip.cpp/h  # Gossip 协议
    ├── replication_mgr.cpp/h # 主从复制
    └── cluster_bus.cpp/h     # 集群总线
```

## 测试

CI 分两层：`ci.yml` 是每次 PR 必须通过的快门禁，`daily.yml` 是夜间重档（sanitizer、长压测、e2e、与真 Redis 的对比、多架构）。测试按 ctest LABELS 分成 `gate`（必过）、`contract`（可见但不拦）、`slow`（只在夜间跑），详见 [docs/testing.md](docs/testing.md)。

### 运行测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure        # 全部
ctest --test-dir build -L gate -j1                # 只跑必过门禁那一层
```

### 单独测试

ctest 用例名注册在 `test/CMakeLists.txt` 的 `CC_TESTS` 表里（名称 | target | LABELS | TIMEOUT），共 16 条：

| `ctest` 用例名 | 说明 | 标签 |
|------|------|------|
| AtomicTests | 原子操作正确性 | gate |
| SyncPrimitivesTests | CountDownLatch/CyclicBarrier | gate |
| ClusterTests | 集群功能 | gate |
| LockCorrectnessTests | Mutex/RWLock/SpinLock 正确性 | gate |
| LockDeadlockTests | 死锁检测 | gate |
| LockRaceTests | 数据竞争检测 | gate |
| LockBoundaryTests | 锁边界条件 | gate |
| V3Tests | 存储/数据类型/持久化/配置等全套单测 | gate |
| ContractTests | 对外行为契约（命令语义） | gate |
| LockRaceDemoTests | 故意制造的竞争演示 | slow |
| ConcurrencyTests | 并发语义（部分断言尚未达标） | contract |
| LockStressTests | 锁压力 | slow |
| StressTest | 高并发压力测试 | slow |
| LoadLimitTest | 容量上限探测 | slow |
| LongRunningStressTest | 长时间稳定性 | slow |
| NetworkStressTest | 连接风暴（独占服务器端口） | slow |

### Sanitizers

```bash
# AddressSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON

# ThreadSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
```

## 版本历史

| 版本 | 状态 | 说明 |
|------|------|------|
| V1.0 | 完成 | 单 Reactor 架构，基础命令 |
| V2.0 | 完成 | MainSubReactor 分离，内存池，锁机制 |
| V3.0 | 完成 | LIST/HASH/SET/ZSET 数据类型 |
| V3.1 | 完成 | RDB 持久化 |
| V4.0 | 完成 | 集群模式，Gossip 协议，主从复制 |

## 编译选项

```bash
# Release 模式
cmake .. -DCMAKE_BUILD_TYPE=Release

# Debug 模式
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Sanitizers
cmake .. -DENABLE_ASAN=ON
cmake .. -DENABLE_TSAN=ON
cmake .. -DENABLE_UBSAN=ON
```

## 文档

完整文档位于 [`docs/`](docs/README.md)：

| 主题 | 链接 |
|------|------|
| 架构总览 | [docs/architecture/overview.md](docs/architecture/overview.md) |
| 网络层 | [docs/architecture/network.md](docs/architecture/network.md) |
| 存储层 | [docs/architecture/storage.md](docs/architecture/storage.md) |
| 内存池 | [docs/architecture/memory-pool.md](docs/architecture/memory-pool.md) |
| 持久化 | [docs/architecture/persistence.md](docs/architecture/persistence.md) |
| 集群 | [docs/architecture/cluster.md](docs/architecture/cluster.md) |
| API 命令 | [docs/api.md](docs/api.md) |
| 测试 | [docs/testing.md](docs/testing.md) |
| 部署运维 | [docs/deployment.md](docs/deployment.md) |

## 设计决策

### 分段锁哈希表

GlobalStorage 将哈希表分为 64 个分片，每个分片独立加锁。高并发场景下，操作分散到不同分片，显著降低锁竞争。

### 三层内存池

- ThreadCache：线程本地缓存，无锁分配，延迟最低
- CentralCache：跨线程内存协调，细粒度锁
- PageCache：直接与系统交互，大块内存分配

### ARU 淘汰算法

近似 LRU，通过 `last_access_time_ms` 实现，**由写入路径同步触发**：`set`/`mutate` 在拿分片锁之前先查 `size()`，一旦占用率 ≥ `max_entries × 0.9` 就淘汰到 `max_entries × 0.6`。单轮淘汰最多 1024 个 key 并每 64 个复查一次 `size()`；每次淘汰随机采样一个分片（最多试 8 个分片），在片内优先删已过期的 key，否则删该片最久未访问的 key。后台 `ExpirationChecker` 每 100ms 只负责按 TTL 删除过期键，不参与淘汰。

## 参考资料

- [Redis 设计与实现](https://github.com/huangz1990/redisbook)
- [muduo 网络库](https://github.com/chenshuo/muduo)
- [Linux 高性能服务器编程](https://book.douban.com/subject/24772279/)
- [RESP 协议规范](https://redis.io/topics/protocol)

## Star History

<a href="https://www.star-history.com/?repos=zimingttkx%2FConcurrentCache&type=date&logscale=&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=zimingttkx/ConcurrentCache&type=date&theme=dark&logscale&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=zimingttkx/ConcurrentCache&type=date&logscale&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=zimingttkx/ConcurrentCache&type=date&logscale&legend=top-left" />
 </picture>
</a>

## 许可证

MIT License - 详见 [LICENSE](LICENSE)
