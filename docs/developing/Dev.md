# ConcurrentCache 开发路线图

## 文档说明

本文档记录项目的**完整开发历程**和**未来规划**。每次完成一个模块，就在对应位置更新状态。

---

## 开发时间线总览

```
【已完成】
═══════════════════════════════════════════════════════════════════════════════

2026-04-24 之前 ─── V1 骨架版本开发完成
  ├── ✅ Logger（基础版）
  ├── ✅ Config（基础版）
  ├── ✅ Signal（基础版）
  ├── ✅ Socket/Buffer/Channel
  ├── ✅ EventLoop（单Reactor）
  ├── ✅ Connection
  ├── ✅ RESP 协议
  ├── ✅ CommandFactory
  └── ✅ GlobalStorage + GET/SET/DEL/EXISTS

2026-04-24 ~ 04-29 ─── V2 基础版本开发
  ├── ✅ Logger（Sink + 模块 + 热加载）
  ├── ✅ Config（观察者 + 热加载接口）
  ├── ✅ Format（统一格式化工具）
  ├── ✅ Signal（SIGSEGV堆栈 + SIGPIPE）
  ├── ✅ 锁机制（Mutex/SpinLock/RWLock/Semaphore等）
  ├── ✅ 内存池（三级分层：ThreadCache/CentralCache/PageCache）
  ├── ✅ MainSubReactor（MainReactor + SubReactor + SubReactorPool）
  ├── ✅ 线程池（通用线程池 + future 返回）
  ├── ✅ 优雅退出（修复信号处理死锁问题）
  └── ✅ 测试套件（锁测试 + 原子测试 + 同步原语测试）

2026-05-07 ─── V3 多数据类型扩展 ✅ 已完成
  ├── ✅ CacheObject 统一对象封装
  ├── ✅ LIST 数据结构（LPUSH/RPUSH/LPOP/RPOP/LLEN/LRANGE）
  ├── ✅ HASH 数据结构（HSET/HGET/HDEL/HLEN/HGETALL）
  ├── ✅ SET 数据结构（SADD/SPOP/SCARD/SISMEMBER/SMEMBERS）
  └── ✅ ZSET 数据结构（ZADD/ZSCORE/ZCARD/ZRANGE）

2026-05-09 ─── V3.1 RDB 持久化 ✅ 已完成
  ├── ✅ RDB 文件格式解析和生成
  ├── ✅ RDBScheduler 定时调度器
  ├── ✅ fork 子进程执行 BGSAVE
  ├── ✅ Copy-on-Write 机制
  └── ✅ 启动时自动加载 dump.rdb

2026-05-10 ─── V4 集群版本 🔄 开发中
  ├── ✅ 集群配置解析
  ├── ✅ ClusterNode 节点数据结构
  ├── ✅ ClusterState 集群状态管理
  ├── ✅ ClusterServer 集群服务器入口
  └── ✅ CRC16 槽算法（与 Redis 兼容）

【计划中】
═══════════════════════════════════════════════════════════════════════════════

V4 分布式版本 ─── 集群支持
  ├── 🔄 CLUSTER MEET 命令（进行中）
  ├── 📋 Gossip 协议（计划中）
  ├── 📋 节点握手（计划中）
  ├── 📋 主从复制（计划中）
  └── 📋 故障转移（计划中）
```

---

## V1 骨架版本开发记录

**开发周期**：2026-04-24 之前（约3周）

**核心成果**：一个完整可运行的单线程缓存服务器

### 开发历程

| 日期 | 模块 | 状态 | 做了什么 |
|------|------|------|---------|
| 第1天 | 项目初始化 | ✅ | 创建目录结构、配置CMake |
| 第2-4天 | Logger | ✅ | 基于spdlog实现4级日志 |
| 第2-4天 | Config | ✅ | 单例 + key=value解析 |
| 第2-4天 | Signal | ✅ | atomic_bool + 优雅退出 |
| 第5-7天 | Socket | ✅ | TCP套接字封装 |
| 第5-7天 | EventLoop | ✅ | epoll事件循环 |
| 第8-9天 | Buffer | ✅ | 读写指针分离，解决粘包 |
| 第8-9天 | Channel | ✅ | fd + 事件 + 回调封装 |
| 第10天 | Connection | ✅ | 资源管理 + 读写处理 |
| 第11-13天 | RESP协议 | ✅ | 5种数据类型解析 |
| 第14-16天 | 命令系统 | ✅ | CommandFactory + 4个命令 |
| 第17-18天 | 集成测试 | ✅ | 完整流程验证 |
| 第19-20天 | 验收 | ✅ | redis-cli兼容测试 |

### V1 架构决策

**为什么选择单Reactor？**
```
先解决"能不能跑"的问题
→ 验证核心设计是否正确
→ 为后续迭代打好基础
```

**为什么用 unordered_map？**
```
O(1) 平均复杂度
C++ 标准库自带
足够简单，先能用
```

**为什么用 shared_mutex？**
```
读多写少场景
多个读可以并行
写操作独占
```

### V1 留下的问题

| 问题 | 影响 | V2解决方案 |
|------|------|-----------|
| 单线程只能用1核 | 并发能力受限 | MainSubReactor |
| 全局一把锁 | 高并发锁竞争 | 分段锁哈希表 |
| malloc/free | 性能差、碎片 | 三级内存池 |
| 无过期机制 | 内存无限增长 | 过期字典 |
| 无淘汰策略 | 内存用满无法写 | LRU算法 |

---

## V2 基础版本开发记录

**开发周期**：2026-04-24 开始

**目标**：完善基础工具，添加内存池、线程池、缓存核心功能

### 当前进度

```
Logger        ████████████████████████████  100%  ✅
Config        ████████████████████████████  100%  ✅
Format        ████████████████████████████  100%  ✅
Signal        ████████████████████████████  100%  ✅
锁机制        ████████████████████████████  100%  ✅
内存池        ████████████████████████████  100%  ✅
MainSubReactor ████████████████████████████  100%  ✅
线程池        ████████████████████████████  100%  ✅
优雅退出      ████████████████████████████  100%  ✅
测试套件      ████████████████████████████  100%  ✅
分段锁哈希表  ████████████████████████████  100%  ✅
过期字典+ARU   ████████████████████████████  100%  ✅
TTL命令      ████████████████████████████  100%  ✅
```

### 已完成模块详解

#### ✅ 锁机制（2026-04-26）

**为什么做**：
```
- 多线程访问共享资源需要同步
- 标准库 mutex 功能有限
- 需要多种锁类型应对不同场景
```

**核心功能**：
```cpp
// 原子操作
AtomicInteger counter(0);
counter.fetch_add(1);

// 互斥锁（支持超时）
Mutex mutex;
mutex.try_lock_for(std::chrono::milliseconds(100));

// 自旋锁（适合极短临界区）
SpinLock spinlock;
SpinLockGuard guard(spinlock);

// 读写锁（读多写少场景）
RWLock rwlock;
RWLockReadGuard reader(rwlock);   // 多个读可以并发
RWLockWriteGuard writer(rwlock);  // 写必须独占

// 分片锁（减少锁竞争）
ShardedLock<SpinLock> sharded_locks(16);

// 信号量（控制并发数量）
Semaphore sem(3);  // 最多3个线程同时访问
```

**文件**：`src/base/lock.h`, `src/base/lock.cpp`

#### ✅ 内存池（2026-04-27）

**为什么做**：
```
malloc/free 的问题：
- 系统调用开销（用户态↔内核态切换）
- 锁竞争（所有线程共享堆）
- 内存碎片（小对象频繁分配释放）

性能对比：
- malloc/free: 1000ms（基准）
- tcmalloc: 50ms（20倍提升）
- jemalloc: 60ms（16倍提升）
```

**架构设计**：
```
┌─────────────────────────────────────────────────────────────┐
│                    ThreadCache（第一层）                      │
│  线程本地缓存，无锁分配，极快                                 │
│  每个线程独立，分配时不需要任何锁                              │
└───────────────────────────────┬─────────────────────────────┘
                                │ 缓存不够时，批量获取
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                    CentralCache（第二层）                    │
│  中心缓存，细粒度锁                                          │
│  所有线程共享，每个SizeClass独立锁                             │
└───────────────────────────────┬─────────────────────────────┘
                                │ Span不够时
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                    PageCache（第三层）                       │
│  页缓存，直接和系统交互                                       │
│  管理4KB页，Span合并减少碎片                                  │
└───────────────────────────────┬─────────────────────────────┘
                                │
                                ▼
                         系统（mmap/munmap）
```

**文件**：
- `src/memorypool/size_class.h/cpp` - 大小分类
- `src/memorypool/free_list.h/cpp` - 空闲链表
- `src/memorypool/span.h/cpp` - Span和SpanList
- `src/memorypool/page_cache.h/cpp` - PageCache
- `src/memorypool/central_cache.h/cpp` - CentralCache
- `src/memorypool/thread_cache.h/cpp` - ThreadCache

#### ✅ MainSubReactor（2026-04-27）

**为什么做**：
```
单Reactor问题：
- 只能用到一个CPU核心
- 8核CPU服务器，只有1核在工作

MainSubReactor解决方案：
- MainReactor：只处理accept（单线程）
- SubReactor池：处理I/O（多线程）
- 连接均匀分发到各个SubReactor
```

**架构设计**：
```
┌─────────────────────────────────────────────────────────────────────────┐
│                          MainReactor                                   │
│                      (独立线程 0)                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │  EventLoop                                                       │  │
│  │    listen_channel ──▶ [epoll_wait] ──▶ handle_accept()          │  │
│  │    accept() ──▶ add_new_connection() ──▶ SubReactorPool         │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┬─┘
                                                                          │
                                          新连接分配 (Round Robin)         │
                                                                          │
        ┌────────────────────────────────────────────────────────────────┘
        │
        ▼
┌───────────────────┬───────────────────┬───────────────────┐
│   SubReactor 0   │   SubReactor 1   │   SubReactor 2   │  ...
│   (独立线程)      │   (独立线程)      │   (独立线程)      │
├───────────────────┼───────────────────┼───────────────────┤
│  EventLoop 0     │  EventLoop 1     │  EventLoop 2     │
│  ┌─────────────┐ │ ┌─────────────┐ │ ┌─────────────┐  │
│  │ connections │ │ │ connections │ │ │ connections │  │
│  └─────────────┘ │ └─────────────┘ │ └─────────────┘  │
└───────────────────┴─────────────────┴─────────────────┘
```

**文件**：
- `src/network/main_reactor.h/cpp` - MainReactor
- `src/network/sub_reactor.h/cpp` - SubReactor
- `src/network/sub_reactor_pool.h/cpp` - SubReactorPool

#### ✅ 线程池（2026-04-28）

**为什么做**：
```
每任务一线程的问题：
- 1000个并发请求 = 1000个线程
- 每个线程占用1MB+栈内存
- 线程创建/销毁开销大

线程池解决方案：
- 固定数量工作线程（如4个）
- 所有任务放入阻塞队列
- 工作线程从队列取任务执行
```

**架构设计**：
```
主线程                      工作线程池
  │                             │
  ├─── 提交任务 ──────────► 阻塞队列 ◄── 工作线程1
  │                             │              工作线程2
  │◄─── 返回 future ──────────┘              工作线程3
  │                                         工作线程4
```

#### ✅ 优雅退出（2026-04-29）

**问题**：信号处理函数中调用 join() 导致主线程阻塞，形成死锁

**解决方案**：
```
1. signal_handler 只调用 quit()，不执行阻塞操作
2. quit() 设置 quit_=true 并调用 wakeup() 唤醒 epoll_wait
3. EventLoop 在 epoll_wait 返回后检测 quit_ 并退出循环
4. main() 在 main_reactor.start() 返回后继续执行清理流程
```

#### ✅ 测试套件（2026-04-29）

**为什么做**：
```
- 验证并发工具的正确性
- 检测死锁和数据竞争
- 压力测试验证稳定性
- 边界条件测试
```

**测试分类**：

| 测试 | 功能 |
|------|------|
| test_lock_correctness | Mutex/SpinLock/RWLock 等基本功能测试 |
| test_lock_deadlock | ABBA 死锁检测、多锁循环测试 |
| test_lock_race | 数据竞争检测、无保护并发访问测试 |
| test_lock_stress | 高并发压力测试、读写混合测试 |
| test_lock_boundary | 超时边界、零值、最大并发测试 |
| test_atomic_correctness | 原子操作正确性测试 |
| test_sync_primitives | Semaphore/CountDownLatch/CyclicBarrier 测试 |

---

## V3 多数据类型扩展（✅ 已完成）

**开发周期**：2026-05-07

**目标**：支持更多数据类型（LIST/HASH/SET/ZSET）

### 已完成模块详解

#### ✅ CacheObject 统一对象封装（2026-05-07）

**为什么做**：
```
V2 版本的 GlobalStorage 只能存储字符串
V3 需要支持多种数据类型：
- STRING（原有）
- LIST（列表）
- HASH（哈希表）
- SET（集合）
- ZSET（有序集合）

需要一个统一的类型来封装这些不同的数据结构
```

**核心设计**：
```cpp
enum class ObjectType : uint8_t {
    STRING = 0,
    LIST = 1,
    HASH = 2,
    SET = 3,
    ZSET = 4
};

class CacheObject {
    ObjectType type_;
    std::string string_val_;
    std::vector<std::string> list_val_;
    std::unordered_map<std::string, std::string> hash_val_;
    std::unordered_set<std::string> set_val_;
    std::set<ZSetMember> zset_val_;  // 有序集合
};
```

**文件**：`src/datatype/object.h`, `src/datatype/object.cpp`

#### ✅ LIST 数据结构（2026-05-07）

**底层实现**：`std::vector<std::string>`

**支持命令**：
| 命令 | 功能 | 返回值 |
|------|------|--------|
| LPUSH | 从左侧推入 | 列表长度 |
| RPUSH | 从右侧推入 | 列表长度 |
| LPOP | 从左侧弹出 | 元素或 nil |
| RPOP | 从右侧弹出 | 元素或 nil |
| LLEN | 获取长度 | 长度 |
| LRANGE | 范围查询 | 元素列表 |

**特点**：
- 支持负索引（-1 表示最后一个元素）
- 范围边界自动调整

#### ✅ HASH 数据结构（2026-05-07）

**底层实现**：`std::unordered_map<std::string, std::string>`

**支持命令**：
| 命令 | 功能 | 返回值 |
|------|------|--------|
| HSET | 设置字段 | 1新增/0更新 |
| HGET | 获取字段值 | 值或 nil |
| HDEL | 删除字段 | 删除数量 |
| HLEN | 获取字段数 | 数量 |
| HGETALL | 获取所有键值对 | 键值对列表 |

#### ✅ SET 数据结构（2026-05-07）

**底层实现**：`std::unordered_set<std::string>`

**支持命令**：
| 命令 | 功能 | 返回值 |
|------|------|--------|
| SADD | 添加成员 | 新增数量 |
| SPOP | 随机弹出 | 成员或 nil |
| SCARD | 成员数量 | 数量 |
| SISMEMBER | 成员是否存在 | 1是/0否 |
| SMEMBERS | 获取所有成员 | 成员列表 |

**特点**：
- 使用 `std::mt19937` + `std::uniform_int_distribution` 高质量随机数

#### ✅ ZSET 数据结构（2026-05-07）

**底层实现**：`std::set<ZSetMember>`（按分数排序）

**成员结构**：
```cpp
struct ZSetMember {
    std::string member;
    double score;
    bool operator<(const ZSetMember& other) const {
        if (score < other.score) return true;
        if (score > other.score) return false;
        return member < other.member;  // 分数相同时按字典序
    }
};
```

**支持命令**：
| 命令 | 功能 | 返回值 |
|------|------|--------|
| ZADD | 添加成员 | 新增/更新 |
| ZSCORE | 获取分数 | 分数或 nil |
| ZCARD | 成员数量 | 数量 |
| ZRANGE | 按分数范围查询 | 成员列表（可选 WITHSCORES） |

---

## V3.1 RDB 持久化（✅ 已完成）

**开发时间**：2026-05-09

**为什么做**：
```
- 数据需要持久化保存
- 服务器重启后数据不丢失
- 定时快照 + 脏键阈值触发
```

**架构设计**：
```
┌─────────────────────────────────────────────────────────────┐
│                    RDBScheduler（调度器）                      │
│                                                             │
│  - 定时触发（默认900秒）                                    │
│  - 脏键阈值触发（默认100个）                                │
│  - fork子进程执行快照，不阻塞主进程                          │
└───────────────────────────────┬─────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                    RDBFile（RDB文件）                       │
│                                                             │
│  - 版本号识别                                              │
│  - CRC32 校验                                             │
│  - 支持 STRING/LIST/HASH/SET/ZSET                         │
│  - 启动时自动加载                                          │
└─────────────────────────────────────────────────────────────┘
```

**核心代码**：
```cpp
// RDB 保存触发条件
if (dirty_keys >= config.rdb_dirty_threshold ||
    elapsed_since_last_save >= config.rdb_save_interval) {
    // fork 子进程执行 bgsave
    rdb_scheduler.schedule_save();
}

// 启动时加载
if (file_exists("dump.rdb")) {
    rdb.load("dump.rdb");
}
```

**文件**：
- `src/persistence/rdb.h/cpp` - RDB 文件格式解析和生成
- `src/persistence/rdb_scheduler.h/cpp` - RDB 保存调度器

---

## V4 分布式版本（🔄 开发中）

**开发时间**：2026-05-10 开始

**目标**：支持集群部署，实现数据分片和高可用

### 当前进度

```
集群配置解析      ████████████████████████████  100%  ✅
ClusterNode       ████████████████████████████  100%  ✅
ClusterState      ████████████████████████████  100%  ✅
ClusterServer     ████████████████████████████  100%  ✅
CRC16 槽算法      ████████████████████████████  100%  ✅
单元测试         ████████████████████████████  100%  ✅
CLUSTER MEET     ░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0%  📋
Gossip 协议      ░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0%  📋
节点握手         ░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0%  📋
主从复制         ░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0%  📋
故障转移         ░░░░░░░░░░░░░░░░░░░░░░░░░░░░   0%  📋
```

### 已完成模块详解

#### ✅ 集群配置解析（2026-05-10）

**为什么做**：
```
集群模式需要新的配置项：
- 是否启用集群模式
- 集群配置文件路径
- 节点超时时间
- 副本有效性因子
```

**新增配置项**：
```ini
cluster_enabled = false
cluster_config_file = nodes.conf
cluster_node_timeout = 15000
cluster_replica_validity_factor = 10
cluster_require_full_coverage = false
```

**文件**：
- `src/base/config.h` - 新增集群配置方法
- `src/base/config.cpp` - 新增集群配置实现
- `conf/concurrentcache.conf` - 新增集群配置项

#### ✅ ClusterNode 节点数据结构（2026-05-10）

**为什么做**：
```
集群中每个节点需要存储：
- 节点标识（名称、IP、端口）
- 节点角色（主/从/未知）
- 节点状态标志（故障/握手中/无地址）
- 心跳信息（ping/pong 时间）
- 槽分配信息
- 主从关系
```

**核心设计**：
```cpp
// 节点角色
enum class NodeRole {
    kMaster = 0,      // 主节点
    kReplica = 1,     // 从节点
    kNodeUnknown = 2  // 未知/未定义
};

// 节点标志（可组合）
enum class NodeFlags {
    kNone = 0,
    kFail = 1 << 0,           // 节点已下线
    kPfail = 1 << 1,          // 节点疑似下线
    kHandshake = 1 << 2,      // 节点正在握手
    kNoAddress = 1 << 3,       // 节点地址未知
};

// 节点信息
struct NodeInfo {
    std::string name;               // 节点唯一名称
    std::string ip;                // IP地址
    int port = 0;                  // 端口
    std::string replicaof_ip;       // 主节点IP（副本用）
    int replicaof_port = 0;         // 主节点端口（副本用）
    NodeRole role;                  // 角色
    uint64_t flags = 0;           // 标志
    int64_t ping_sent = 0;        // 发送ping的时间
    int64_t pong_received = 0;    // 收到pong的时间
};
```

**文件**：
- `src/cluster/cluster_common.h` - 公共类型定义
- `src/cluster/cluster_node.h` - ClusterNode 类声明
- `src/cluster/cluster_node.cpp` - ClusterNode 类实现

#### ✅ ClusterState 集群状态管理（2026-05-10）

**为什么做**：
```
集群需要管理所有节点：
- 节点列表（name -> ClusterNode）
- 槽映射表（slot -> ClusterNode）
- 线程安全（shared_mutex）
```

**核心设计**：
```cpp
class ClusterState {
    // 节点管理
    void addNode(std::shared_ptr<ClusterNode> node);
    void delNode(const std::string& name);
    std::shared_ptr<ClusterNode> getNode(const std::string& name) const;

    // 槽管理（存储 shared_ptr 避免二次查找）
    void setNodeForSlot(int slot, std::shared_ptr<ClusterNode> node);
    std::shared_ptr<ClusterNode> getNodeForSlot(int slot) const;

private:
    std::unordered_map<std::string, std::shared_ptr<ClusterNode>> nodes_;
    std::unordered_map<int, std::shared_ptr<ClusterNode>> slots_;  // 直接存储指针
    mutable std::shared_mutex mutex_;
    mutable std::shared_mutex slots_mutex_;
};
```

**特点**：
- 使用 `shared_mutex` 支持多读单写
- 槽映射表直接存储 `shared_ptr<ClusterNode>`，避免二次查找
- `delNode` 时自动清理槽引用
- 防御性编程（assert、nullptr 检查）

**文件**：
- `src/cluster/cluster_state.h` - ClusterState 类声明
- `src/cluster/cluster_state.cpp` - ClusterState 类实现

#### ✅ ClusterServer 集群服务器入口（2026-05-10）

**为什么做**：
```
集群需要一个统一的入口：
- 单例模式
- 初始化（读取配置、创建本节点）
- 启动/停止
- key -> slot 映射
```

**核心设计**：
```cpp
class ClusterServer {
    static ClusterServer& instance();
    void init();    // 初始化
    void start();    // 启动
    void stop();     // 停止

    // 槽算法（与 Redis 兼容）
    int keyToSlot(const std::string& key) const;
    std::shared_ptr<ClusterNode> getNodeByKey(const std::string& key) const;

private:
    bool enabled_ = false;
    std::shared_ptr<ClusterNode> my_node_;
    ClusterState state_;
    std::atomic<bool> running_{false};
};
```

**CRC16 槽算法**：
```
Redis 使用 CRC16 算法将 key 映射到 16384 个槽:
- 支持 {tag} 语法，如 {user:1}:profile 只对 user:1 哈希
- 公式: slot = CRC16(key) & 16383
```

**文件**：
- `src/cluster/cluster_server.h` - ClusterServer 类声明
- `src/cluster/cluster_server.cpp` - ClusterServer 类实现

#### ✅ 单元测试（2026-05-10）

**测试覆盖**：
- ClusterNode 基本功能
- ClusterState 节点管理
- ClusterState 槽映射
- ClusterServer 单例
- keyToSlot 范围验证

**文件**：
- `test/cluster_test/cluster_test.cpp` - 集群模块单元测试

---

## 待开发模块

### 📋 CLUSTER MEET 命令

**目标**：让两个节点能够互相认识

**功能**：
- 节点握手协议
- 节点信息交换
- 节点列表更新

### 📋 Gossip 协议

**目标**：节点间状态传播和故障检测

**功能**：
- PING/PONG 消息
- 节点状态交换
- PFAIL/FAIL 标记

### 📋 主从复制

**目标**：实现数据同步

**功能**：
- 全量同步（PSYNC ? -1）
- 增量同步（REPLCONF ACK offset）
- 复制缓冲区

### 📋 故障转移

**目标**：主节点故障时自动切换

**功能**：
- 故障检测
- 选举投票
- 状态切换

---

## 如何更新本文档

每次完成开发，按以下步骤更新：

### 1. 更新进度条
```
V4 模块名      ████████████████████████  100%  ← 完成后更新
```

### 2. 记录时间线
```markdown
| 日期 | 模块 | 状态 | 做了什么 |
|------|------|------|---------|
| 2026-05-10 | ClusterNode | ✅ | 完成节点数据结构 |
```

### 3. 补充模块详解
在对应版本下添加完成的模块说明。

---

## 更新记录

| 日期 | 版本 | 更新内容 |
|------|------|---------|
| 2026-04-24 之前 | V1 | 完成骨架版本所有模块 |
| 2026-04-24 | V2 | Format + Logger重构 + Config增强 |
| 2026-04-25 | V2 | Signal增强（SIGSEGV堆栈 + SIGPIPE忽略）|
| 2026-04-26 | V2 | 锁机制完成（Mutex/SpinLock/RWLock等）|
| 2026-04-27 | V2 | 内存池三级架构完成 |
| 2026-04-27 | V2 | MainSubReactor多线程网络模型完成 |
| 2026-04-28 | V2 | 线程池完成 |
| 2026-05-05 | V2 | 分段锁哈希表完成（64分片） |
| 2026-05-05 | V2 | 过期字典 + ARU淘汰 + TTL命令完成 |
| 2026-05-07 | V3 | 多数据类型扩展完成（LIST/HASH/SET/ZSET）|
| 2026-05-09 | V3.1 | RDB持久化完成 |
| 2026-05-10 | V4 | 集群基础模块完成（配置+Node+State+Server）|
