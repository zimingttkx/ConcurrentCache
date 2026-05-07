# ConcurrentCache 项目开发手册

## 文档说明

本文档是项目的**实时开发进度记录**。每次完成一个模块，就在对应位置更新状态。

**阅读方式**：
- 想快速了解当前进度 → 看「开发进度总览」
- 想了解某个模块的为什么 → 看「已完成模块详解」
- 想了解下一步做什么 → 看「待开发模块」
- 想回顾项目历程 → 看「迭代时间线」

---

## 开发进度总览

```
【基础工具】
┌─────────────────────────────────────────────────────────────┐
│ ✅ Logger        ████████████████████████████ 100%       │
│    - 单例模式 + 异步写入 + 文件轮转                          │
│    - v2.0新增: Sink抽象 + 模块分类 + 配置热加载            │
│                                                             │
│ ✅ Config        ████████████████████████████ 100%        │
│    - 单例模式 + key=value解析 + 观察者模式                   │
│    - v2.0完成: 热加载触发机制（定时检查配置文件修改时间）    │
│                                                             │
│ ✅ Signal        ████████████████████████████ 100%        │
│    - SIGINT/SIGTERM 优雅退出                               │
│    - v2.0完成: SIGSEGV堆栈 + SIGPIPE忽略                  │
│                                                             │
│ ✅ Format        ████████████████████████████ 100%        │
│    - 统一格式化工具                                         │
└─────────────────────────────────────────────────────────────┘

【并发工具】
┌─────────────────────────────────────────────────────────────┐
│ ✅ 锁机制       ████████████████████████████ 100%         │
│    - AtomicInteger/AtomicPointer（原子操作）                 │
│    - Mutex（支持超时）                                     │
│    - SpinLock（自旋锁，指数退避）                          │
│    - RecursiveMutex（递归锁）                              │
│    - RWLock/RWLock2（读写锁，写优先）                      │
│    - Semaphore/CountDownLatch/CyclicBarrier（同步原语）    │
│    - ShardedLock/ShardedRWLock（分片锁）                   │
│    - LockGuard/TryLockGuard/ReadLockGuard/WriteLockGuard  │
└─────────────────────────────────────────────────────────────┘

【网络通信层】
┌─────────────────────────────────────────────────────────────┐
│ ✅ Socket       ████████████████████████████ 100%         │
│ ✅ Buffer       ████████████████████████████ 100%         │
│ ✅ Channel      ████████████████████████████ 100%         │
│ ✅ EventLoop    ████████████████████████████ 100%         │
│ ✅ Connection   ████████████████████████████ 100%         │
│                                                             │
│ ✅ MainSubReactor ████████████████████████████ 100%       │
│    - MainReactor（专门处理accept）                          │
│    - SubReactor（处理已连接socket I/O，每个独立线程）       │
│    - SubReactorPool（SubReactor管理器，轮询负载均衡）       │
└─────────────────────────────────────────────────────────────┘

【内存管理】
┌─────────────────────────────────────────────────────────────┐
│ ✅ 内存池     ████████████████████████████ 100%             │
│    - ThreadCache（线程本地，无锁）                          │
│    - CentralCache（细粒度锁）                              │
│    - PageCache（全局锁，Span合并）                         │
└─────────────────────────────────────────────────────────────┘

【线程池】
┌─────────────────────────────────────────────────────────────┐
│ ✅ 线程池     ████████████████████████████ 100%             │
│    - 固定数量工作线程                                       │
│    - 阻塞队列 + 条件变量                                   │
│    - future 返回值                                         │
│    - 优雅退出                                             │
└─────────────────────────────────────────────────────────────┘

【缓存核心】
┌─────────────────────────────────────────────────────────────┐
│ ✅ GlobalStorage ████████████████████████████ 100%         │
│    - unordered_map + shared_mutex                          │
│                                                             │
│ ✅ 分段锁哈希表 ████████████████████████████ 100%             │
│ ✅ 过期字典     ████████████████████████████ 100%            │
│ ✅ ARU淘汰      ████████████████████████████ 100%            │
└─────────────────────────────────────────────────────────────┘

【协议与命令】
┌─────────────────────────────────────────────────────────────┐
│ ✅ RESP协议   ████████████████████████████ 100%           │
│ ✅ CommandFactory ████████████████████████████ 100%       │
│                                                             │
│ ✅ STRING命令 ████████████████████████████ 100%           │
│    - GET/SET/DEL/EXISTS/PING                             │
│                                                             │
│ ✅ TTL命令    ████████████████████████████ 100%            │
│    - EXPIRE/TTL/PTTL/PERSIST/SETEX                        │
│                                                             │
│ ✅ LIST命令   ████████████████████████████ 100%            │
│    - LPUSH/RPUSH/LPOP/RPOP/LLEN/LRANGE                    │
│    - 支持负索引                                            │
│                                                             │
│ ✅ HASH命令   ████████████████████████████ 100%            │
│    - HSET/HGET/HDEL/HLEN/HGETALL                          │
│                                                             │
│ ✅ SET命令    ████████████████████████████ 100%            │
│    - SADD/SPOP/SCARD/SISMEMBER/SMEMBERS                   │
│    - 高质量随机数（mt19937）                               │
│                                                             │
│ ✅ ZSET命令   ████████████████████████████ 100%            │
│    - ZADD/ZSCORE/ZCARD/ZRANGE                             │
│    - std::set 有序存储，支持 WITHSCORES                   │
└─────────────────────────────────────────────────────────────┘

【测试】
┌─────────────────────────────────────────────────────────────┐
│ ✅ 测试框架   ████████████████████████████ 100%           │
│    - GTest风格断言                                         │
│    - TraceLogger/TraceAnalyzer                            │
│                                                             │
│ ✅ 锁测试     ████████████████████████████ 100%           │
│    - 正确性测试                                            │
│    - 死锁检测测试                                          │
│    - 数据竞争测试                                          │
│    - 压力测试                                             │
│    - 边界测试                                             │
│                                                             │
│ ✅ 原子操作测试 ████████████████████████████ 100%         │
│                                                             │
│ ✅ 同步原语测试 ████████████████████████████ 100%         │
└─────────────────────────────────────────────────────────────┘

图例: ✅ 已完成  🔄 进行中  📋 计划中
```

---

## 迭代时间线

| 日期 | 模块 | 状态 | 关键改动 |
|------|------|------|---------|
| 2026-04-24 之前 | 骨架版本所有模块 | ✅ | 完成基本服务器实现 |
| 2026-04-24 | Logger | ✅ | Sink抽象 + 模块支持 + 热加载代码完成 |
| 2026-04-24 | Config | ✅ | 观察者模式 + reload() 代码完成 |
| 2026-04-24 | Format | ✅ 新增 | 统一格式化工具类 |
| 2026-04-25 | Signal | ✅ | SIGSEGV堆栈捕获 + SIGPIPE忽略 完成 |
| 2026-04-25 | Config | ✅ | 热加载触发机制集成（定时检查配置）|
| 2026-04-26 | 锁机制 | ✅ 新增 | Mutex/SpinLock/RWLock等完整实现 |
| 2026-04-27 | 内存池 | ✅ 新增 | ThreadCache/CentralCache/PageCache三级架构 |
| 2026-04-27 | MainSubReactor | ✅ 新增 | MainReactor + SubReactor + SubReactorPool |
| 2026-04-28 | 线程池 | ✅ 新增 | ThreadPool 完整实现 |
| 2026-04-29 | 优雅退出 | ✅ 修复 | 修复信号处理导致的死锁问题 |
| 2026-04-29 | 测试套件 | ✅ 新增 | 锁测试、原子测试、同步原语测试 |
| 2026-05-05 | 缓存核心 | ✅ | 分段锁哈希表 + 过期字典 + ARU淘汰 + TTL命令 |
| 2026-05-07 | V3数据类型 | ✅ 新增 | CacheObject + LIST/HASH/SET/ZSET 命令实现 |

---

## 已完成模块详解

### ✅ Logger（日志系统）

**开发时间**：2026-04-24（v2.0 重构）

**问题**：版本1只有控制台输出，无法按模块过滤，无法热加载

**为什么这样设计**：

```
1. 为什么需要 Sink 抽象？
   - 旧设计：输出硬编码在 Logger 里
   - 问题：想同时输出到文件和控制台？得改 Logger 源码
   - 新设计：Logger 只管队列，输出由 Sink 决定
   - 好处：可以自由组合，随时加新输出方式

2. 为什么需要模块分类？
   - 问题：所有日志混在一起，难以定位问题
   - 解决：每个日志带模块名 [NETWORK] [CACHE] [STORAGE]
   - 好处：可以只看特定模块的日志

3. 为什么需要热加载？
   - 问题：修改日志级别需要重启服务
   - 解决：Logger 实现 ConfigObserver，配置文件改了就自动更新
```

**核心代码**：
```cpp
// Sink 抽象
class Sink {
    virtual void write(const std::string& msg) = 0;
    virtual void flush() = 0;
};

// ConsoleSink + FileSink 实现

// 带模块的日志
#define LOG_INFO(module, fmt, ...) \
    Logger::instance().log(#module, LogLevel::INFO, fmt, ##__VA_ARGS__)

LOG_INFO(NETWORK, "connection accepted fd=%d", fd);
// 输出: [INFO] 2026-04-24 15:30:00.123 [NETWORK] [12345] connection accepted fd=5
```

**输出格式**：
```
[LEVEL] timestamp [MODULE] [THREAD_ID] message
[INFO] 2026-04-24 15:30:00.123 [NETWORK] [12345] connection accepted fd=5
[DEBUG] 2026-04-24 15:30:00.456 [CACHE] [12346] get key=user:1
[ERROR] 2026-04-24 15:30:01.789 [STORAGE] [12345] write failed: no space
```

**文件**：`src/base/log.h`, `src/base/log.cpp`

---

### ✅ Config（配置系统）

**开发时间**：2026-04-24（v2.0 增强）

**问题**：版本1没有热加载，没有观察者模式

**为什么这样设计**：

```
1. 为什么需要观察者模式？
   - 问题：Config 变化时，相关模块需要知道
   - 解决：Observer 模式，Config 变化通知观察者
   - 好处：解耦，Config 不需要知道谁关心它

2. 为什么需要热加载？
   - 问题：改配置要重启服务
   - 解决：reload() 重新读取文件，notifyObservers() 通知变化
```

**核心代码**：
```cpp
// ConfigObserver 接口
class ConfigObserver {
    virtual void onConfigChange(const std::string& key, const std::string& value) = 0;
};

// 使用
Config::instance().addObserver("log_level", &Logger::instance());

// 配置热加载
void reload() {
    // 重新读取配置
    // 通知所有观察者
    for (auto& [key, observers] : observers_) {
        for (auto* obs : observers) {
            obs->onConfigChange(key, values_[key]);
        }
    }
}
```

**文件**：`src/base/config.h`, `src/base/config.cpp`

---

### ✅ Format（格式化工具）

**开发时间**：2026-04-24（v2.0 新增）

**为什么单独一个类**：
- 统一格式化逻辑，避免散落各处
- 时间戳、线程ID 可复用
- Logger、Format 各司其职

**核心代码**：
```cpp
class Format {
    static std::string format(const char* fmt, ...);  // 可变参数格式化
    static std::string timestamp();                    // 毫秒精度时间戳
    static std::string threadId();                   // 线程ID
};
```

**文件**：`src/base/format.h`, `src/base/format.cpp`

---

### ✅ Signal（信号处理）

**开发时间**：骨架版本 + 2026-04-25（v2.0 增强完成）

**v2.0 完成内容**：
- SIGINT/SIGTERM 优雅退出
- SIGSEGV 堆栈捕获（使用 backtrace + dladdr + abi::__cxa_demangle）
- SIGPIPE 忽略（避免向关闭连接写入崩溃）

**为什么这样设计**：
```
1. 为什么用 atomic_bool？
   - 信号处理函数在独立上下文执行
   - atomic 保证多线程/信号间安全通信

2. 为什么只设置标志位？
   - 信号处理函数不能做复杂操作
   - 标志位让主循环检测并处理退出

3. 为什么需要 SIGSEGV 捕获？
   - 段错误发生时打印调用栈，便于定位问题
   - 使用 backtrace() 获取地址，dladdr() + __cxa_demangle 还原函数名

4. 为什么忽略 SIGPIPE？
   - 对端关闭连接时写数据，操作系统发 SIGPIPE
   - 忽略后 write() 返回 EPIPE，可以从容处理
```

**文件**：`src/base/signal.h`, `src/base/signal.cpp`

---

### ✅ 锁机制（并发工具）

**开发时间**：2026-04-26（v2.0 新增）

**为什么做**：
```
- 多线程访问共享资源需要同步
- 标准库 mutex 功能有限
- 需要多种锁类型应对不同场景
```

**核心组件**：

```cpp
// 1. 原子操作
AtomicInteger counter(0);
counter.fetch_add(1);

// 2. 互斥锁（支持超时）
Mutex mutex;
mutex.try_lock_for(std::chrono::milliseconds(100));

// 3. 自旋锁（适合极短临界区，指数退避）
SpinLock spinlock;
SpinLockGuard guard(spinlock);

// 4. 递归锁（同一线程可多次获取）
RecursiveMutex rmutex;
RecursiveMutexGuard rguard(rmutex);

// 5. 读写锁（读多写少场景）
RWLock rwlock;
RWLockReadGuard reader(rwlock);   // 多个读可以并发
RWLockWriteGuard writer(rwlock);  // 写必须独占

// 6. 改进读写锁（写优先，避免写饥饿）
RWLock2 rwlock2;
RWLock2WriteGuard wguard(rwlock2);

// 7. 分片锁（减少锁竞争）
ShardedLock<SpinLock> sharded_locks(16);
auto& lock = sharded_locks.get_shard(key);

// 8. 信号量（控制并发数量）
Semaphore sem(3);  // 最多3个线程同时访问
sem.wait();
sem.post();

// 9. 倒计时门栓
CountDownLatch latch(3);
latch.count_down();
latch.wait();

// 10. 循环屏障
CyclicBarrier barrier(3);
barrier.wait();
```

**文件**：`src/base/lock.h`, `src/base/lock.cpp`

---

### ✅ 内存池（三级分层）

**开发时间**：2026-04-27（v2.0 新增）

**为什么做**：
```
malloc/free 的问题：
- 系统调用开销（用户态↔内核态切换）
- 锁竞争（所有线程共享堆）
- 内存碎片（小对象频繁分配释放）

性能对比：
- malloc/free: 1000ms（基准）
- tcmalloc: 50ms（20倍提升）
```

**架构设计**：
```
┌─────────────────────────────────────────────────────────────┐
│                    ThreadCache（第一层）                      │
│  线程本地缓存，无锁分配，极快                                 │
│  每个线程独立，分配时不需要任何锁                              │
│  批量获取/归还，减少锁操作次数                               │
└───────────────────────────────┬─────────────────────────────┘
                                │ 缓存不够时，批量获取
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                    CentralCache（第二层）                    │
│  中心缓存，细粒度锁                                          │
│  所有线程共享，每个SizeClass独立锁                             │
│  管理Span，切分小块                                          │
└───────────────────────────────┬─────────────────────────────┘
                                │ Span不够时
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                    PageCache（第三层）                       │
│  页缓存，直接和系统交互                                       │
│  管理4KB页，Span合并减少碎片                                  │
│  全局锁，向系统申请/释放内存                                  │
└─────────────────────────────────────────────────────────────┘
```

**文件**：
- `src/memorypool/size_class.h/cpp` - 29个固定大小档次
- `src/memorypool/free_list.h/cpp` - 空闲链表（头插头取O(1)）
- `src/memorypool/span.h/cpp` - Span和SpanList
- `src/memorypool/page_cache.h/cpp` - PageCache
- `src/memorypool/central_cache.h/cpp` - CentralCache
- `src/memorypool/thread_cache.h/cpp` - ThreadCache
- `src/memorypool/memorypool.md` - 详细架构文档

---

### ✅ MainSubReactor（多线程网络模型）

**开发时间**：2026-04-27（v2.0 新增）

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
│  │  0-999     │ │ │  1000-1999 │ │ │  2000-2999 │  │
│  └─────────────┘ │ └─────────────┘ │ └─────────────┘  │
└───────────────────┴─────────────────┴─────────────────┘
```

**核心组件**：
- `MainReactor`：只负责 accept 新连接
- `SubReactor`：每个拥有独立 EventLoop，处理已连接 socket 的 I/O
- `SubReactorPool`：管理所有 SubReactor，轮询负载均衡

**文件**：
- `src/network/main_reactor.h/cpp` - MainReactor
- `src/network/sub_reactor.h/cpp` - SubReactor
- `src/network/sub_reactor_pool.h/cpp` - SubReactorPool

---

### ✅ 线程池

**开发时间**：2026-04-28（v2.0 新增）

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
  ├─── 提交任务 ──────────► 队列 ◄─── 工作线程
  │                             │
  │◄─── 返回 future ────────────┘
```

**核心代码**：
```cpp
// 提交任务
auto future = thread_pool.submit([]() {
    return 1 + 2;
});
int result = future.get();

// 停止线程池
thread_pool.stop();
```

**文件**：`src/base/thread_pool.h`, `src/base/thread_pool.cpp`

---

### ✅ 优雅退出

**开发时间**：2026-04-29（v2.0 修复）

**问题**：信号处理函数中调用 join() 导致主线程阻塞，形成死锁

**解决方案**：
```
1. signal_handler 只调用 quit()，不执行阻塞操作
2. quit() 设置 quit_=true 并调用 wakeup() 唤醒 epoll_wait
3. EventLoop 在 epoll_wait 返回后检测 quit_ 并退出循环
4. main() 在 main_reactor.start() 返回后继续执行清理流程
```

**退出流程**：
```
1. 收到 SIGINT/SIGTERM
2. signal_handler() 调用 g_main_reactor->event_loop()->quit()
3. quit() 设置 quit_=true 并调用 wakeup()
4. MainReactor 的 EventLoop 检测到 quit_，退出 epoll_wait
5. main() 继续执行，依次调用：
   - SubReactorPool::stop() - 停止所有 SubReactor
   - MainReactor::stop()   - 停止 MainReactor
   - ThreadPool::stop()    - 停止线程池
6. 进程安全退出
```

---

### ✅ 测试套件

**开发时间**：2026-04-29（v2.0 新增）

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

**文件**：
- `test/trace/test_assertions.h` - GTest 风格断言
- `test/trace/trace_logger.h` - 锁跟踪
- `test/trace/trace_analyzer.h` - 死锁/竞争检测
- `test/lock_test/*.cpp` - 锁测试
- `test/atomic_test/*.cpp` - 原子测试
- `test/sync_primitives_test/*.cpp` - 同步原语测试

---

## 待开发模块

### ✅ 分段锁哈希表

**问题**：全局一把锁，高并发成为瓶颈

**架构设计**：
```
┌────────┬────────┬────────┬────────┐
│分片0锁 │分片1锁 │分片2锁 │分片3锁 │ ...
└────────┴────────┴────────┴────────┘
┌──────┐│┌──────┐│┌──────┐│┌──────┐│
│分片0  │││分片1  │││分片2  │││分片3  ││
│key0   │││key1   │││key2   │││key3   ││
│key4   │││key5   │││key6   │││key7   ││
└──────┘│└──────┘│└──────┘│└──────┘│
线程A访问分片0，线程B访问分片1 → 完全并行
```

**目标**：并发读写性能提升 5 倍+

---

### ✅ 过期字典 + ARU淘汰

**问题**：数据永不过期，内存无限增长

**架构设计**：
```
过期字典：key → expire_time(ms)
    │
    ├── 惰性删除：访问时检查，过期就删
    └── 定期删除：后台线程每100ms随机检查20个键
    │
LRU链表：访问时移动到头部，淘汰时从尾部删
```

---

### ✅ 多数据类型扩展（V3已完成）

**实现内容**：
- CacheObject 统一对象封装（STRING/LIST/HASH/SET/ZSET）
- LIST: LPUSH/RPUSH/LPOP/RPOP/LLEN/LRANGE（支持负索引）
- HASH: HSET/HGET/HDEL/HLEN/HGETALL
- SET: SADD/SPOP/SCARD/SISMEMBER/SMEMBERS（mt19937随机）
- ZSET: ZADD/ZSCORE/ZCARD/ZRANGE（std::set有序）

**文件**：
- `src/datatype/object.h/cpp` - CacheObject 定义与实现

---

### 📋 持久化增强（V3下一阶段）

**待实现**：
```
RDB持久化：定时快照，fork子进程不阻塞主进程
AOF持久化：追加日志，重启重放
```

---

### 📋 进阶命令

**待实现命令**：
```
LIST: LSET/LINDEX/LINSERT/LREM
HASH: HVALS/HKEYS
SET: SREM
ZSET: ZREM/ZINCRBY/ZRANK/ZREVRANGE
```

---

## 项目结构

```
concurrentcache/
├── src/
│   ├── base/
│   │   ├── log.h/cpp          ✅ Sink抽象 + 模块 + 热加载
│   │   ├── format.h/cpp       ✅ 统一格式化
│   │   ├── config.h/cpp      ✅ 单例 + 观察者 + 热加载
│   │   ├── signal.h/cpp       ✅ SIGSEGV + SIGPIPE
│   │   ├── lock.h/cpp         ✅ 完整锁机制
│   │   └── thread_pool.h/cpp  ✅ 通用线程池
│   │
│   ├── network/
│   │   ├── socket.h
│   │   ├── buffer.h
│   │   ├── channel.h
│   │   ├── event_loop.h/cpp
│   │   ├── connection.h/cpp
│   │   ├── main_reactor.h/cpp    ✅ MainReactor
│   │   ├── sub_reactor.h/cpp     ✅ SubReactor
│   │   └── sub_reactor_pool.h/cpp ✅ SubReactorPool
│   │
│   ├── memorypool/
│   │   ├── size_class.h/cpp      ✅ 大小分类
│   │   ├── free_list.h/cpp      ✅ 空闲链表
│   │   ├── span.h/cpp           ✅ Span
│   │   ├── page_cache.h/cpp     ✅ PageCache
│   │   ├── central_cache.h/cpp  ✅ CentralCache
│   │   ├── thread_cache.h/cpp   ✅ ThreadCache
│   │   ├── memory_pool.h        ✅ 统一入口
│   │   └── memorypool.md        ✅ 架构文档
│   │
│   ├── protocol/
│   │   └── resp.h/cpp
│   │
│   ├── datatype/
│   │   ├── object.h           ✅ V3 统一对象封装
│   │   └── object.cpp         ✅ CacheObject 实现
│   │
│   ├── command/
│   │   ├── command.h
│   │   ├── string_cmd.h        ✅ GET/SET/DEL/EXISTS/PING + TTL + LIST/HASH/SET/ZSET
│   │   └── command_factory.h/cpp
│   │
│   └── cache/
│       └── storage.h/cpp        ✅ 全局存储（V3适配 CacheObject）
│
├── test/
│   ├── trace/                      ✅ 测试框架
│   ├── lock_test/                  ✅ 锁测试
│   ├── atomic_test/                ✅ 原子测试
│   └── sync_primitives_test/       ✅ 同步原语测试
│
├── docs/
│   └── developing/
│       ├── Dev.md              ← 本文档
│       ├── Architecture.md      ← 架构文档
│       └── Roadmap.md          ← 开发路线图
│
└── conf/
    └── concurrentcache.conf
```

---

## 更新记录

| 日期 | 模块 | 状态 | 更新内容 |
|------|------|------|---------|
| 2026-04-24 之前 | 骨架版本所有模块 | ✅ | 完成基本服务器实现 |
| 2026-04-24 | Logger | ✅ | Sink抽象 + 模块支持 + 热加载代码完成 |
| 2026-04-24 | Config | ✅ | 观察者模式 + reload() 代码完成 |
| 2026-04-24 | Format | ✅ 新增 | 统一格式化工具类 |
| 2026-04-25 | Signal | ✅ | SIGSEGV堆栈捕获 + SIGPIPE忽略 功能完成 |
| 2026-04-25 | Config | ✅ | 热加载触发机制集成（EventLoop定时检查）|
| 2026-04-26 | 锁机制 | ✅ 新增 | Mutex/SpinLock/RWLock等完整实现 |
| 2026-04-27 | 内存池 | ✅ 新增 | ThreadCache/CentralCache/PageCache三级架构 |
| 2026-04-27 | MainSubReactor | ✅ 新增 | MainReactor + SubReactor + SubReactorPool |
| 2026-04-28 | 线程池 | ✅ 新增 | ThreadPool 完整实现 |
| 2026-04-29 | 优雅退出 | ✅ 修复 | 修复信号处理导致的死锁问题 |
| 2026-04-29 | 测试套件 | ✅ 新增 | 锁测试、原子测试、同步原语测试 |
| 2026-05-05 | 缓存核心 | ✅ | 分段锁哈希表 + 过期字典 + ARU淘汰 + TTL命令 |
| 2026-05-07 | V3数据类型 | ✅ 新增 | CacheObject + LIST/HASH/SET/ZSET 命令实现 |
