# 架构总览

> **范围**：ConcurrentCache V3.0 系统分层、组件依赖、请求处理时序。
> **适用读者**：所有工程师。
> **前置阅读**：[项目 README](../../README.md)。
> **深入阅读**：[网络层](./network.md) · [存储层](./storage.md) · [持久化](./persistence.md) · [集群](./cluster.md)。

## 1. 设计目标

| 目标 | 指标 | 落地手段 |
|------|------|---------|
| 高并发 | 与 Redis 7.0 平均 QPS 比率 90.4%（8 核混合负载） | MainSubReactor 多线程 + 64 分片 shared_mutex |
| 低延迟 | GET P99 ≤ 1ms（并发 ≤ 100） | 线程本地数据、惰性过期、epoll LT 模式 |
| 数据安全 | 重启不丢数据 | RDB 周期快照 + 优雅退出强制保存 |
| 横向扩展 | 多节点分片 | 16384 哈希槽 + Gossip + 主从复制 |
| 协议兼容 | Redis 客户端直连 | RESP 2.0 解析/编码 |

## 2. 系统分层

```
┌─────────────────────────────────────────────┐
│              客户端 (redis-cli / SDK)         │
├─────────────────────────────────────────────┤
│  网络层 (src/network/)                       │
│  MainReactor → SubReactorPool → EventLoop   │
│  Connection + Buffer + Channel              │
├─────────────────────────────────────────────┤
│  协议层 (src/protocol/)                      │
│  RespParser / RespEncoder (RESP 2.0)        │
├─────────────────────────────────────────────┤
│  命令层 (src/command/)                       │
│  CommandFactory + 44 个 Command 子类          │
├─────────────────────────────────────────────┤
│  缓存层 (src/cache/ + src/datatype/)         │
│  GlobalStorage(64分片) + CacheObject(5类型)   │
│  ExpireDict + ExpirationChecker             │
├─────────────────────────────────────────────┤
│  持久化层 (src/persistence/)                 │
│  RdbPersistence + RdbScheduler              │
├─────────────────────────────────────────────┤
│  集群层 (src/cluster/)  [可选]                │
│  ClusterServer + Gossip + ReplicationMgr    │
├─────────────────────────────────────────────┤
│  基础设施 (src/base/)                         │
│  Logger / Config / ThreadPool / Signal / Lock │
└─────────────────────────────────────────────┘
```

## 3. 模块清单

| 层 | 模块 | 关键类 | 职责 |
|----|------|--------|------|
| 入口 | `main.cpp` | `main()` | 信号注册 → 加载配置 → 启动组件 → 阻塞 MainReactor |
| 网络 | `src/network/` | `MainReactor`、`SubReactorPool`、`EventLoop`、`Connection`、`Channel`、`Buffer` | 端口监听、连接分发、I/O 多路复用、读写缓冲 |
| 协议 | `src/protocol/` | `RespParser`、`RespEncoder` | RESP 2.0 解析/编码 |
| 命令 | `src/command/` | `Command` 基类、`CommandFactory` 单例 | 44 个命令注册、参数校验、调用存储层 |
| 存储 | `src/cache/` | `GlobalStorage`、`CacheObject`、`ExpireDict`、`ExpirationChecker` | 64 分片哈希表、5 数据类型、过期管理、ARU 随机分片采样淘汰（单轮上限 1024） |
| 持久化 | `src/persistence/` | `RdbPersistence`、`RdbScheduler` | RDB 写入/读取、自动保存调度 |
| 集群 | `src/cluster/` | `ClusterServer`、`ClusterState`、`ClusterNode`、`ClusterBus`、`ClusterGossip`、`ReplicationMgr` | 槽位管理、Gossip、主从复制、故障转移 |
| 基础设施 | `src/base/` | `Logger`、`Config`、`ThreadPool`、`Signal`、`lock.cpp` | 跨层通用组件 |

> **内存池模块**（`src/memorypool/`）已实现三级池化（ThreadCache/CentralCache/PageCache），当前编译进二进制但**未被其他模块调用**，所有分配走系统 `malloc/free`。启用方法见 [memory-pool.md](./memory-pool.md)。

## 4. 启动流程

`main.cpp` 严格按照以下顺序初始化：

1. `setrlimit(RLIMIT_NOFILE)` 提到 65535（支撑大量并发连接）
2. 注册 `SIGINT` / `SIGTERM` 信号处理器
3. 加载 `conf/concurrentcache.conf`（`rdb_save_interval ≤ 0` 等非法值自动回退默认）
4. 初始化日志系统
5. 初始化 + 启动 `SubReactorPool`（N 个 I/O 线程）
6. 创建通用 `ThreadPool`（异步任务）
7. 初始化 `MainReactor`（创建 listen socket）
8. **加载 RDB**（`RdbPersistence::load`）— 必须在 `ExpirationChecker` 启动前完成
9. 初始化 `ClusterServer`（先 `set_event_loop`，再 `init()`）
10. 启动 `ExpirationChecker`（100ms 周期）
11. 启动 `RdbScheduler`（时间+阈值触发）
12. 启动 `ClusterServer`（如启用）
13. `MainReactor::start()` — **阻塞**，进入 epoll 循环

**优雅退出**（SIGINT/SIGTERM 到达）：

1. `signal_handler` 设 `g_running = false`（async-signal-safe：仅 atomic exchange + `write(STDERR)` + `EventLoop::quit()`）
2. 主 loop 退出，`main_reactor.start()` 返回
3. 顺序停止：`RdbScheduler` → `SubReactorPool`（quit + join 全部 SubReactor）→ `MainReactor` → `ExpirationChecker` → `ClusterServer` → `ThreadPool`
4. **强制 `RdbPersistence::save`**（保证最后写不丢）
5. 进程退出

**关键不变量**：

1. `GlobalStorage::instance()` 必须在 `RdbPersistence::load()` 之前可用
2. `SubReactorPool` 必须先于 `MainReactor` 启动
3. RDB 加载必须在 `ExpirationChecker` 启动前完成
4. 线程池必须**最后**停止（其他组件可能有排队任务）

## 5. 请求处理时序（以 GET key 为例）

```
Client ──TCP──► MainReactor (accept)
                  │
                  ▼
             SubReactorPool::get_next_reactor() (轮询，atomic fetch_add)
                  │
                  ▼
             SubReactor i (epoll LT)
                  │
                  ▼
             Connection::handle_read()
              ├── recv() → input_buffer_
              ├── has_protocol_error? → 回错 + close（协议级，断开）
              ├── RespParser::parse() → RespValue（parse 错误 → 回错 + reset，可恢复）
              └── CommandCallback → GetCommand（执行 try/catch 兜底）
                    │
                    ▼
             GlobalStorage::get(key)
              ├── hash(key) % 64 → shard_index
              ├── mutexes_[shard].lock_shared()
              ├── stores_[shard].find(key)
              ├── expire_dict_.is_expired? → 删除并返回 nil
              └── return optional<CacheObject>
                    │
                    ▼
             RespEncoder::encode_bulk_string() / encode_nil()
              └── output_buffer_ → write() → client
```

**关键路径耗时**（Release 构建、8 核）：

| 阶段 | 耗时 |
|------|------|
| TCP accept + 分发 | < 10 μs |
| RESP 解析（短命令） | < 5 μs |
| CommandFactory::create（map 查找） | < 1 μs |
| GlobalStorage::get（读锁 + hash 查找） | < 1 μs |
| RESP 编码 | < 5 μs |
| 写 socket | < 10 μs |
| **合计** | **< 50 μs** |

## 6. 关键不变量

| 不变量 | 维护机制 |
|--------|---------|
| `GlobalStorage` 全局唯一 | Magic Static 单例 + `delete` 拷贝构造 |
| 每分片独立加锁 | 64 把 `std::shared_mutex` + 哈希分片 |
| `Connection` 生命周期 ≤ `EventLoop` | `SubReactor` 持有 `unique_ptr<Connection>`；关闭走「先回调后关 fd」防泄漏 |
| 畸形输入不杀进程 | 协议错误回错断开；命令执行/事件分发双层 try/catch 兜底 |
| `Command` 实例独立 | `CommandFactory` 存储模板 + `clone()` 创建新实例 |
| 过期键最终被删除 | 惰性删除（get 时）+ 周期删除（100ms 周期、25ms 预算内反复抽样）双重保证 |
| 写入 RDB 前不丢数据 | 周期快照（.tmp+rename 原子写）+ 优雅退出强制保存 |
| 信号处理不阻塞 | `signal_handler` 仅做 atomic exchange + `write()` + `quit()`（async-signal-safe） |
| WRONGTYPE 保护 | 所有类型敏感命令执行前检查 `CacheObject::type()` |

## 7. 性能特征

以下数据基于 **8 核 Linux x86_64、Release 构建、64 分片、对比 Redis 7.0.15** 实测：

| 场景 | ConcurrentCache | Redis | CC/Redis |
|------|----------------|-------|----------|
| 纯 GET（单连接） | 32,863 QPS | 55,224 QPS | 60% |
| 纯 SET（单连接） | 42,318 QPS | 55,099 QPS | 77% |
| 并发=100 混合（7:2:1） | 85,430 QPS | 87,734 QPS | 97% |
| 并发=1000 混合 | 75,425 QPS | 75,438 QPS | 100% |
| 并发=5000 混合 | 57,073 QPS | 62,301 QPS | 92% |
| 功能正确性 | **38/39 (97.4%)** | 38/39 (97.4%) | — |

> 完整性能报告见 [deployment.md § 11](../deployment.md)。对比测试脚本见 `test/e2e_test/comparison_test.py`。

## 8. 部署形态

| 形态 | 组件裁剪 | 启动配置 |
|------|---------|---------|
| **单实例** | 全部模块，`cluster_enabled=false` | 默认配置即可 |
| **集群** | `ClusterServer` 启用 Gossip + Bus + Replication | `cluster_enabled=true` |

## 9. 另见

- [网络层详解](./network.md)
- [存储层详解](./storage.md)
- [持久化详解](./persistence.md)
- [集群详解](./cluster.md)
- [内存池详解](./memory-pool.md)
- [API 命令手册](../api.md)
- [测试文档](../testing.md)
- [部署与运维](../deployment.md)
