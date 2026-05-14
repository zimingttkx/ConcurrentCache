# ConcurrentCache

> 一个从零开始实现的 C++ 高性能内存缓存系统，兼容 Redis RESP 协议

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

## 项目简介

ConcurrentCache 是一个纯手工实现的 C++20 内存对象缓存系统，完全兼容 Redis RESP 协议，可直接使用 `redis-cli` 或任何 Redis 客户端连接操作。

### 核心特性

| 特性 | 说明 |
| --- | --- |
| **高性能网络模型** | MainReactor + SubReactorPool 多线程 Reactor 架构，基于 epoll 多路复用 |
| **线程安全存储** | 分段锁哈希表（64 分片），大幅降低锁竞争 |
| **高效内存管理** | 三层内存池：ThreadCache（无锁）→ CentralCache（细粒度锁）→ PageCache |
| **Redis 协议兼容** | 支持 STRING/LIST/HASH/SET/ZSET 五种数据类型 |
| **持久化支持** | RDB 快照，Fork/COW 机制，服务重启自动恢复 |
| **优雅退出** | 信号处理机制，确保连接安全关闭和数据完整保存 |
| **集群支持** | V4 规划中：哈希槽分片、Gossip 协议、主从复制 |

### 与 Redis 的关系

本项目是**学习项目**，旨在通过从零实现一个类 Redis 缓存系统，深入理解：

- Linux 高性能网络编程（epoll/Reactor 模式）
- 并发控制（锁、无锁数据结构、内存模型）
- 缓存系统核心设计（存储结构、淘汰策略、过期管理）
- 分布式系统基础（Gossip 协议、复制、故障转移）

> **注意**：本项目并非 Redis 的替代品，而是一个教学性质的实现。

---

## 架构设计

### 整体架构

```text
┌─────────────────────────────────────────────────────────────────┐
│                         Client Layer                             │
│                  redis-cli / jedis / redis-py                   │
└─────────────────────────────┬───────────────────────────────────┘
                              │ TCP (RESP Protocol)
┌─────────────────────────────▼───────────────────────────────────┐
│                       MainReactor                                │
│              (单线程 · 处理新连接 accept)                       │
└─────────────────────────────┬───────────────────────────────────┘
                              │ 轮询分发
┌─────────────────────────────▼───────────────────────────────────┐
│                    SubReactorPool                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │SubReactor│  │SubReactor│  │SubReactor│  │SubReactor│   ...  │
│  │ Thread 0 │  │ Thread 1 │  │ Thread 2 │  │ Thread N │        │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │
│       │             │             │             │               │
│   ┌───▼───┐     ┌───▼───┐     ┌───▼───┐     ┌───▼───┐        │
│   │EventLoop│    │EventLoop│    │EventLoop│    │EventLoop│       │
│   │(epoll) │    │(epoll) │    │(epoll) │    │(epoll) │        │
│   └────────┘    └────────┘    └────────┘    └────────┘        │
└─────────────────────────────────────────────────────────────────┘
```

### 核心模块

| 层级 | 模块 | 职责 |
|------|------|------|
| **网络层** | MainReactor | 监听端口，处理新连接 |
| | SubReactorPool | 管理多个 SubReactor 线程，轮询负载均衡 |
| | EventLoop | 基于 epoll 的事件循环，I/O 就绪通知 |
| | Connection | TCP 连接管理，收发缓冲区 |
| | Buffer | 数据缓冲区，解决 TCP 粘包问题 |
| **缓存层** | GlobalStorage | 分段锁哈希表（64 分片），键值存储 |
| | ExpireDict | 过期键管理，支持 TTL/PTTL/PERSIST |
| | ExpirationChecker | 后台线程定期清理过期键 |
| **内存池** | ThreadCache | 线程本地缓存，完全无锁分配 |
| | CentralCache | 中心缓存，跨线程内存协调 |
| | PageCache | 页缓存，直接与系统交互 |
| **命令层** | CommandFactory | 命令工厂，统一命令创建和管理 |
| | StringCmd / ListCmd / ... | 各数据类型命令实现 |
| **持久化层** | RDB | Redis Dump 文件格式，快照持久化 |
| | RDBScheduler | Fork 进程执行快照，COW 优化 |

---

## 技术规格

| 项目 | 规格 |
|------|------|
| **语言标准** | C++20 |
| **目标平台** | Linux (x86_64) |
| **构建系统** | CMake 3.20+ |
| **网络模型** | MainReactor + SubReactorPool (epoll LT) |
| **线程数** | 可配置，默认 32 个 SubReactor |
| **依赖库** | ZLIB (RDB 压缩) |
| **协议兼容** | Redis RESP 2.0 / RESP 3.0 |

---

## 支持的命令

### STRING

| 命令 | 说明 |
|------|------|
| `GET key` | 获取值 |
| `SET key value` | 设置值 |
| `DEL key [key ...]` | 删除键 |
| `EXISTS key [key ...]` | 检查键是否存在 |
| `PING` | 心跳检测 |
| `EXPIRE key seconds` | 设置过期时间（秒） |
| `TTL key` | 获取剩余生存时间（秒） |
| `PTTL key` | 获取剩余生存时间（毫秒） |
| `PERSIST key` | 移除过期时间 |
| `SETEX key seconds value` | 设置值并指定过期时间 |

### LIST

| 命令 | 说明 |
|------|------|
| `LPUSH key value [value ...]` | 左侧推入 |
| `RPUSH key value [value ...]` | 右侧推入 |
| `LPOP key` | 左侧弹出 |
| `RPOP key` | 右侧弹出 |
| `LLEN key` | 获取长度 |
| `LRANGE key start stop` | 范围查询（支持负索引） |

### HASH

| 命令 | 说明 |
|------|------|
| `HSET key field value [field value ...]` | 设置字段 |
| `HGET key field` | 获取字段值 |
| `HDEL key field [field ...]` | 删除字段 |
| `HLEN key` | 获取字段数量 |
| `HGETALL key` | 获取所有字段和值 |

### SET

| 命令 | 说明 |
|------|------|
| `SADD key member [member ...]` | 添加成员 |
| `SPOP key [count]` | 随机弹出 |
| `SCARD key` | 获取成员数量 |
| `SISMEMBER key member` | 检查成员是否存在 |
| `SMEMBERS key` | 获取所有成员 |

### ZSET

| 命令 | 说明 |
|------|------|
| `ZADD key score member [score member ...]` | 添加成员及分数 |
| `ZSCORE key member` | 获取成员分数 |
| `ZCARD key` | 获取成员数量 |
| `ZRANGE key start stop [WITHSCORES]` | 按索引范围查询 |

### RDB

| 命令 | 说明 |
|------|------|
| `SAVE` | 同步保存快照 |
| `BGSAVE` | 后台异步保存快照 |
| `LASTSAVE` | 获取上次保存时间戳 |

---

## 快速开始

### 环境要求

- Linux (x86_64)
- GCC 12+ / Clang 16+ / MSVC 2022+
- CMake 3.20+
- ZLIB development libraries

### 编译

```bash
git clone https://github.com/dingziming/ConcurrentCache.git
cd ConcurrentCache
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 启动服务器

```bash
./concurrentcache-server
# 默认监听 0.0.0.0:6379
```

### 使用 redis-cli 测试

```bash
redis-cli -p 6379

127.0.0.1:6379> PING
PONG

127.0.0.1:6379> SET name concurrentcache
OK

127.0.0.1:6379> GET name
"concurrentcache"

127.0.0.1:6379> HSET user:1 name Alice age 25
(integer) 2

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

### 配置文件

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

---

## 项目结构

```
src/
├── base/                       # 基础组件
│   ├── log.cpp/h              # 日志系统 (多级别输出、Sink 抽象)
│   ├── config.cpp/h           # 配置管理 (热加载支持)
│   ├── signal.cpp/h           # 信号处理 (SIGSEGV 堆栈捕获)
│   ├── format.cpp/h           # 字符串格式化
│   ├── lock.cpp/h             # 锁机制 (Mutex/SpinLock/RWLock)
│   └── thread_pool.cpp/h      # 通用线程池
│
├── network/                    # 网络层
│   ├── socket.cpp/h           # Socket 封装
│   ├── event_loop.cpp/h       # 事件循环 (epoll)
│   ├── channel.cpp/h          # 事件通道
│   ├── connection.cpp/h        # 连接管理
│   ├── buffer.cpp/h           # 收发的缓冲区
│   ├── main_reactor.cpp/h     # MainReactor
│   ├── sub_reactor.cpp/h      # SubReactor
│   └── sub_reactor_pool.cpp/h # SubReactor 池
│
├── memorypool/                 # 内存池 (三层设计)
│   ├── memory_pool.h          # 统一接口
│   ├── size_class.cpp/h       # Size Class 计算
│   ├── thread_cache.cpp/h     # 线程本地缓存
│   ├── central_cache.cpp/h    # 中心缓存
│   ├── page_cache.cpp/h       # 页缓存
│   ├── span.cpp/h             # Span 管理
│   └── free_list.cpp/h        # 空闲链表
│
├── protocol/                   # 协议层
│   └── resp.cpp/h             # Redis RESP 协议解析/编码
│
├── datatype/                   # 数据类型
│   └── object.cpp/h           # CacheObject 统一对象封装
│
├── command/                    # 命令层
│   ├── command.h              # 命令基类
│   ├── command_factory.cpp/h  # 命令工厂
│   ├── string_cmd.h           # String 命令
│   ├── list_cmd.h             # List 命令
│   ├── hash_cmd.h             # Hash 命令
│   ├── set_cmd.h              # Set 命令
│   ├── zset_cmd.h             # ZSet 命令
│   ├── cluster_cmd.cpp/h      # 集群命令
│   ├── psync_cmd.cpp/h        # 主从同步命令
│   └── restore_cmd.cpp/h      # 恢复命令
│
├── cache/                      # 缓存核心
│   ├── storage.cpp/h          # GlobalStorage (64 分片哈希表)
│   ├── expire_dict.cpp/h      # 过期字典
│   └── expiration_checker.cpp/h # 过期检查器
│
├── persistence/                # 持久化层
│   ├── rdb.cpp/h             # RDB 文件格式
│   └── rdb_scheduler.cpp/h   # 快照调度器 (Fork/COW)
│
└── cluster/                   # 集群层 (V4)
    ├── cluster_common.h       # 节点角色、标志定义
    ├── cluster_node.cpp/h    # 节点数据结构
    ├── cluster_state.cpp/h   # 集群状态管理
    ├── cluster_server.cpp/h  # 集群服务器入口
    ├── cluster_link.cpp/h    # 节点间链接
    ├── cluster_connection.cpp/h # 连接管理
    ├── cluster_gossip.cpp/h  # Gossip 协议
    ├── replication_mgr.cpp/h # 主从复制管理
    └── cluster_bus.cpp/h    # 集群总线

test/                          # 测试套件
├── atomic_test/              # 原子操作测试
├── lock_test/                # 锁测试 (正确性/死锁/竞争)
├── sync_primitives_test/     # 同步原语测试
├── storage_test/             # 存储测试
├── datatype_test/            # 数据类型测试
├── persistence_test/        # RDB 持久化测试
├── cluster_test/             # 集群功能测试
├── stress_test/             # 压力测试
├── network_test/            # 网络压力测试
└── e2e_test/               # 端到端测试 (Python)

conf/                          # 配置
└── concurrentcache.conf      # 配置文件

docs/architecture/            # 架构文档
├── Architecture_V3.md
├── Architecture_V4.md
├── Roadmap_V3.md
└── Roadmap_V4.md
```

---

## 测试

### 运行测试套件

```bash
# 所有测试
ctest --test-dir build --output-on-failure

# 特定测试
./build/test/atomic-tests
./build/test/lock-correctness-tests
./build/test/storage-test
./build/test/datatype-test
./build/test/cluster-test
./build/test/stress-test
```

### 测试覆盖场景

| 测试 | 说明 |
|------|------|
| `atomic-tests` | AtomicInteger 等原子类正确性验证 |
| `lock-correctness-tests` | Mutex/RWLock/SpinLock 正确性 |
| `lock-deadlock-tests` | 潜在死锁检测 |
| `lock-race-tests` | 数据竞争检测 (ThreadSanitizer) |
| `sync-primitives-tests` | CountDownLatch/CyclicBarrier 等 |
| `storage-test` | GlobalStorage 增删改查 |
| `datatype-test` | 五种数据类型正确性 |
| `persistence-test` | RDB 读写与恢复 |
| `cluster-test` | 集群功能 |
| `stress-test` | 高并发压力测试 |
| `network-stress-test` | 网络 I/O 压力测试 |
| `e2e_test/*.py` | Python 端到端测试 |

---

## 版本历史

| 版本 | 状态 | 分支 | 说明 |
|------|------|------|------|
| V1.0 | ✅ 完成 | version1 | 单 Reactor 架构，基础 GET/SET/DEL |
| V2.0 | ✅ 完成 | version2 | MainSubReactor 分离，内存池，锁机制，TTL |
| V3.0 | ✅ 完成 | version3 | LIST/HASH/SET/ZSET 数据类型 |
| V3.1 | ✅ 完成 | version3 | RDB 持久化，Fork/COW，自动保存 |
| V4.0 | ✅ 完成 | main | 集群模式，Gossip 协议，主从复制 |

---

## 编译选项

```bash
# Release 模式 (推荐)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Debug 模式
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 启用 AddressSanitizer
cmake .. -DENABLE_ASAN=ON

# 启用 ThreadSanitizer
cmake .. -DENABLE_TSAN=ON

# 启用 UndefinedBehaviorSanitizer
cmake .. -DENABLE_UBSAN=ON
```

---

## 设计决策

### 1. 分段锁哈希表

GlobalStorage 将哈希桶分为 64 个分片，每个分片拥有独立的锁。在高并发场景下，大部分操作集中在单个分片，显著降低了锁竞争。

```
存储结构: 64 个独立分片
├── Shard[0]: Mutex + vector<KV>
├── Shard[1]: Mutex + vector<KV>
└── Shard[63]: Mutex + vector<KV>
```

### 2. 三层内存池

```
ThreadCache (CPU L1/L2 缓存级别)
    │ 无锁分配，极低延迟
    ▼
CentralCache (CPU L3 缓存级别)
    │ 细粒度锁，按 Size Class 管理
    ▼
PageCache (系统内存级别)
    │ 直接与 OS 交互，大块内存分配
```

### 3. ARU 淘汰算法

近似 LRU (Approximate LRU)，通过 `last_access_time_ms` 实现。每 100ms 抽样检查过期键时，顺便淘汰最久未访问的键。

---

## 参考资料

- [Redis 设计与实现](https://github.com/huangz1990/redisbook)
- [muduo 网络库](https://github.com/chenshuo/muduo)
- [Linux 高性能服务器编程](https://book.douban.com/subject/24772279/)
- [RESP 协议规范](https://redis.io/topics/protocol)

---

## 许可证

MIT License - 详见 [LICENSE](LICENSE)
