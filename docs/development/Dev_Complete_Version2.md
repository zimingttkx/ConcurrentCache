# 第三章：基础版本（Version2）

## 版本概述

**版本目标**：在骨架版本基础上，完善基础工具，添加完整内存池、线程池和缓存核心功能。

**问题分析**：骨架版本解决了"能跑"的问题，但存在以下性能瓶颈：

```
骨架版本的问题分析：
┌─────────────────────────────────────────────────────────────┐
│  问题1：单Reactor只能利用一个CPU核心                         │
│  ─────────────────────────────────────────────────         │
│  骨架版本的EventLoop是单线程的                              │
│  → 即使服务器有8个CPU核心，也只有1个在工作                  │
│  → 处理能力受限于单核性能                                   │
│                                                              │
│  问题2：malloc/free在高并发下性能差                         │
│  ─────────────────────────────────────────────────         │
│  每次内存分配/释放都要系统调用                              │
│  → 大量线程同时分配内存时，锁竞争严重                       │
│  → 系统调用本身也有开销                                     │
│                                                              │
│  问题3：全局锁成为瓶颈                                      │
│  ─────────────────────────────────────────────────         │
│  GlobalStorage使用一把全局互斥锁保护                        │
│  → 多线程并发读写时，所有线程都要抢这一把锁                 │
│  → 锁竞争严重，并发性能反而下降                            │
│                                                              │
│  问题4：没有过期淘汰机制                                    │
│  ─────────────────────────────────────────────────         │
│  缓存数据永不过期                                          │
│  → 内存会无限增长                                          │
│  → 无法支持带过期时间的缓存场景                            │
└─────────────────────────────────────────────────────────────┘
```

**为什么需要基础版本**：

| 组件 | 解决的问题 | 带来的价值 |
|-----|----------|-----------|
| **完善日志** | 骨架版只有控制台输出 | 日志可持久化、滚动、异步写入 |
| **配置热加载** | 修改配置需重启服务 | 线上灵活调整参数 |
| **完善信号处理** | 崩溃无堆栈信息 | 快速定位问题根因 |
| **各种锁机制** | 多线程并发访问共享资源 | 线程安全、性能可控 |
| **三级内存池** | 频繁malloc/free性能差 | 减少系统调用，高并发下性能提升3倍+ |
| **线程池** | 每个任务创建线程开销大 | 复用线程，高效处理异步任务 |
| **MainSubReactor** | 单Reactor处理能力有限 | 支持万级并发连接 |
| **分段锁哈希表** | 全局锁成为瓶颈 | 并发读写性能提升5倍+ |
| **过期删除** | 数据永不过期 | 支持TTL、内存自动清理 |
| **LRU算法** | 内存无限增长 | 内存可控、淘汰策略灵活 |

**预计开发周期**：4-6周

---

## 3.1 基础版本架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         基础版本架构                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │
│  │   Logger    │  │   Config    │  │   Signal    │  │   Lock     │ │
│  │  （完善版）  │  │  （热加载）  │  │  （完整版）  │  │ (各种锁)   │ │
│  └─────────────┘  └─────────────┘  └─────────────┘  └────────────┘ │
│                                                                     │
│  ┌───────────────────────┐    ┌───────────────────────────────────┐ │
│  │     Memory Pool        │    │         Thread Pool                │ │
│  │  ┌─────┐ ┌─────┐ ┌───┐ │    │  ┌─────┐ ┌─────┐ ┌─────┐ ┌────┐ │ │
│  │  │TLS  │→│Central│→│Page│ │    │  │Worker│→│Worker│→│Worker│→│... │ │ │
│  │  └─────┘ └─────┘ └───┘ │    │  └─────┘ └─────┘ └─────┘ └────┘ │ │
│  └───────────────────────┘    └───────────────────────────────────┘ │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     MainSubReactor网络模型                    │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │   │
│  │  │MainReactor│→│SubReactor│→│SubReactor│→│SubReactor│   │   │
│  │  │(接受连接) │  │(I/O处理) │  │(I/O处理) │  │(I/O处理) │   │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     缓存核心层                                │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │   │
│  │  │ 分段锁   │→│ 过期字典 │→│ LRU算法  │→│ 内存上限 │   │   │
│  │  │ 哈希表   │  │ (TTL)    │  │          │  │          │   │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3.2 章节1：基础工具增强

### 3.2.1 日志系统完善

#### 问题分析

```
骨架版本的问题：
┌─────────────────────────────────────────────────────────────┐
│  只有控制台输出                                            │
│  → 程序重启后日志消失，无法追溯历史问题                    │
│  → 无法分类输出（网络模块、缓存模块）                      │
│  → 无法控制日志级别（开发时需要debug，生产需要info）        │
└─────────────────────────────────────────────────────────────┘
```

#### 为什么需要完善日志系统？

| 特性 | 解决的问题 | 带来的价值 |
|-----|----------|-----------|
| **文件输出** | 控制台日志重启丢失 | 日志可持久化，可追溯历史 |
| **日志滚动** | 日志文件无限增长 | 防止磁盘空间耗尽 |
| **多级别输出** | 所有日志都输出太杂乱 | 可按级别过滤，只看重要的 |
| **多模块分类** | 难以定位问题来源 | 可只看特定模块的日志 |
| **异步写入** | 同步写日志影响性能 | 日志写入不阻塞主线程 |

#### 日志滚动的必要性

```
不滚动的情况：
concurrentcache.log → 100GB → 磁盘写满，程序崩溃

滚动后的情况：
concurrentcache.log      → 当前日志
concurrentcache.log.1    → 最近一次轮转的日志
concurrentcache.log.2    → 更早的日志
...
concurrentcache.log.10   → 保留最近10个文件，旧的自动删除
```

#### 实现要点

```cpp
// 日志系统包含：
// - Logger类：主logger，支持文件和控制台输出
// - FileSink：文件输出，支持滚动
// - ConsoleSink：控制台输出
// - async_logger：异步日志写入

// 配置项：
// - log_file：日志文件路径
// - log_level：日志级别（trace/debug/info/warn/error）
// - log_max_size：单个日志文件最大大小
// - log_max_files：保留的日志文件数量
```

#### 类设计

```cpp
class Logger {
public:
    enum Level { TRACE, DEBUG, INFO, WARN, ERROR, FATAL };

    void set_level(Level level);
    void add_sink(std::shared_ptr<Sink> sink);

    void log(Level level, const char* file, int line,
             const char* module, const char* fmt, ...);

private:
    Level level_;
    std::vector<std::shared_ptr<Sink>> sinks_;
};

class FileSink : public Sink {
private:
    std::string filename_;          // 日志文件名
    size_t max_size_;               // 单文件最大大小
    size_t max_files_;              // 保留文件数
    std::ofstream file_;            // 当前文件流
    std::mutex mutex_;              // 保护文件操作

    void rotate();  // 滚动日志
};
```

#### 验收标准

- 日志能同时输出到控制台和文件
- 日志文件能按配置自动滚动
- 多线程环境下日志输出不混乱
- 异步写入不影响主线程性能

---

### 3.2.2 配置系统完善

#### 问题分析

```
骨架版本的问题：
┌─────────────────────────────────────────────────────────────┐
│  配置硬编码在代码中                                        │
│  → 修改配置需要重新编译                                   │
│  → 不同环境（开发、测试、生产）需要不同配置                 │
│  → 无法动态调整运行时参数                                  │
└─────────────────────────────────────────────────────────────┘
```

#### 为什么需要配置热加载？

| 场景 | 说明 |
|-----|------|
| **动态调整日志级别** | 发现问题后，把日志级别从info改成debug |
| **调整内存上限** | 发现内存使用接近上限，需要调整 |
| **修改线程数** | 根据负载调整线程池大小 |
| **切换淘汰策略** | 根据业务特点调整缓存淘汰策略 |

#### 热加载的使用场景

```
场景1：动态调整日志级别
# 发现问题后，把日志级别从info改成debug
# 修改concurrentcache.conf: log_level = debug
# 配置系统检测到变更，自动通知logger模块

场景2：调整内存上限
# 发现内存使用接近上限，需要调整
# 修改concurrentcache.conf: max_memory = 2147483648  # 2GB
# 配置系统通知cache模块，缓存模块重新计算淘汰策略
```

#### 为什么需要观察者模式？

```
传统方式（不好）：
┌─────────────────────────────────────────────────────────────┐
│  void load_config() {                                     │
│      log_level = config["log_level"];                     │
│      max_memory = config["max_memory"];                   │
│      thread_num = config["thread_num"];                   │
│  }                                                        │
│                                                              │
│  问题：                                                    │
│  - 每次读取都要解析配置文件                                │
│  - 无法感知配置变化                                        │
│  - 调用方不知道配置已更新                                  │
└─────────────────────────────────────────────────────────────┘

观察者模式（好）：
┌─────────────────────────────────────────────────────────────┐
│  class ConfigObserver {                                    │
│  public:                                                   │
│      virtual void on_config_change(key, value) = 0;       │
│  };                                                        │
│                                                              │
│  class Logger : public ConfigObserver {                    │
│  public:                                                   │
│      void on_config_change(key, value) override {         │
│          if (key == "log_level") {                        │
│              set_level(parse(value));                      │
│          }                                                 │
│      }                                                     │
│  };                                                        │
│                                                              │
│  class CacheManager : public ConfigObserver { ... };       │
│                                                              │
│  Config类：                                                 │
│  - register_observer(key, observer)                       │
│  - 当key对应的值变化时，通知所有observer                    │
└─────────────────────────────────────────────────────────────┘
```

#### 主要配置项

```
# 网络配置
port = 6379
bind = 0.0.0.0
max_clients = 10000

# 线程配置
thread_num = 4

# 日志配置
log_level = info
log_file = ./logs/concurrentcache.log

# 缓存配置
max_memory = 1073741824  # 1GB
eviction_policy = lru

# 持久化配置
save_interval = 3600
```

#### 实现要点

```cpp
class Config {
public:
    // 基础类型获取
    int get_int(const std::string& key, int default_val);
    std::string get_string(const std::string& key, const std::string& default_val);
    bool get_bool(const std::string& key, bool default_val);

    // 热加载
    void reload();  // 重新读取配置文件

    // 观察者模式
    void add_observer(const std::string& key, ConfigObserver* observer);
    void remove_observer(const std::string& key, ConfigObserver* observer);

private:
    std::map<std::string, std::string> values_;
    std::map<std::string, std::vector<ConfigObserver*>> observers_;
    std::mutex mutex_;
};
```

#### 验收标准

- 能正确读取所有配置项
- 修改配置文件后能自动重新加载
- 未配置的项能使用默认值
- 配置热加载时不会影响运行中的服务

---

### 3.2.3 信号处理完善

#### 问题分析

```
骨架版本的问题：
┌─────────────────────────────────────────────────────────────┐
│  信号处理缺失或简陋                                        │
│  → 程序崩溃时没有堆栈信息，难以定位问题                    │
│  → 向已关闭连接写入导致SIGPIPE崩溃                        │
│  → 无法优雅退出，正在处理的请求被强制中断                  │
└─────────────────────────────────────────────────────────────┘
```

#### 为什么需要完善信号处理？

| 信号 | 场景 | 处理方式 |
|-----|------|---------|
| **SIGSEGV** | 内存越界访问 | 打印堆栈信息，帮助定位 |
| **SIGINT/SIGTERM** | Ctrl+C / kill | 优雅退出，等待请求处理完 |
| **SIGPIPE** | 向关闭的socket写 | 忽略，避免崩溃 |

#### SIGSEGV堆栈信息的价值

```
没有堆栈信息：
[2024-01-15 10:30:15] [FATAL] Segmentation fault
# 只能知道程序崩溃了，不知道在哪里

有堆栈信息：
[2024-01-15 10:30:15] [FATAL] Segmentation fault
#0  0x00007f4a3c2e1234 in __memmove_avx_unaligned_erms ()
#1  0x00007f4a3c1a5678 in std::string::replace ()
#2  0x0000562a3b4c5678 in HashTable::set (key=0x562a3b4d1234, value=...)
#3  0x0000562a3b4c7890 in SetCommand::execute (args=...)
#  可以精确定位到HashTable::set的第2行
```

#### 实现要点

```cpp
class SignalHandler {
public:
    // 注册信号处理函数
    static void register_handler(int sig, SignalCallback cb);

    // 默认信号处理
    static void handle_sigint(int sig);   // 优雅退出
    static void handle_sigterm(int sig);  // 优雅退出
    static void handle_sigsegv(int sig);   // 打印堆栈
    static void handle_sigpipe(int sig);   // 忽略
};

class StackTrace {
public:
    static void print(std::ostream& os);
};
```

#### 验收标准

- 程序收到SIGINT/SIGTERM信号后能优雅退出
- 收到SIGSEGV信号后能打印堆栈信息
- SIGPIPE不会导致程序崩溃

---

### 3.2.4 锁机制实现

#### 为什么需要各种锁？

多线程并发访问共享资源时，需要锁来保证数据一致性。不同场景需要不同的锁：

| 锁类型 | 适用场景 | 特点 |
|-------|---------|------|
| **Mutex** | 一般互斥 | 简单，易死锁 |
| **SpinLock** | 短临界区 | 忙等，不释放CPU |
| **RWLock** | 读多写少 | 读并行，写独占 |

#### Mutex vs SpinLock

```
Mutex（互斥锁）：
线程A获得锁
线程B等待锁...（挂起，不消耗CPU）
线程A释放锁
线程B被唤醒，获得锁

SpinLock（自旋锁）：
线程A获得锁
线程B忙等... while(lock-held) （消耗CPU）
线程A释放锁
线程B获得锁

选择建议：
- 临界区 > 1000行指令 → 用Mutex
- 临界区 < 100行指令 → 用SpinLock
- 临界区不确定 → 用Mutex
```

#### 为什么需要RAII锁守卫？

```cpp
// 不使用LockGuard（危险）
mutex_.lock();
if (condition) {
    mutex_.unlock();  // 容易忘记unlock
    return;
}
// ... 很多代码
mutex_.unlock();

// 使用LockGuard（安全）
{
    LockGuard<Mutex> guard(mutex_);  // 自动unlock
    if (condition) {
        return;  // 自动unlock
    }
    // ... 很多代码
}  // 作用域结束时自动unlock
```

#### 实现要点

```cpp
// Mutex：基本互斥锁
class Mutex {
public:
    void lock();
    void unlock();
private:
    std::mutex mutex_;
};

// SpinLock：自旋锁
class SpinLock {
public:
    void lock();
    void unlock();
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// RWLock：读写锁
class RWLock {
public:
    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();
private:
    std::shared_mutex mutex_;
};

// RAII锁守卫
template<typename Lockable>
class LockGuard {
public:
    explicit LockGuard(Lockable& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
private:
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    Lockable& lock_;
};
```

#### 验收标准

- 所有锁机制在多线程环境下都正确
- 能正确保护共享资源
- 读写锁支持多个读并发、一个写独占
- 自旋锁在短临界区性能优于互斥锁

---

## 3.3 章节2：内存池实现

### 3.3.1 问题分析

#### 为什么malloc/free在高并发下性能差？

```
malloc 的工作原理（简化）：
┌─────────────────────────────────────────────────────────────┐
│  1. 检查是否有空闲内存块                                    │
│  2. 如果有，分配并返回                                     │
│  3. 如果没有，向操作系统申请（brk/mmap）                  │
│  4. 操作系统切换到内核态，分配内存                        │
│  5. 切换回用户态，返回                                     │
│                                                              │
│  问题：                                                    │
│  - 系统调用开销大（用户态↔内核态切换）                     │
│  - 锁竞争严重（所有线程共享堆）                           │
│  - 内存碎片多（小对象频繁分配释放）                        │
└─────────────────────────────────────────────────────────────┘

性能对比（多线程场景）：
┌─────────────────────────────────────────────────────────────┐
│  方案              │ 100万次分配/释放  │ 相对性能           │
│  ─────────────────────────────────────────────────          │
│  malloc/free       │ 1000ms            │ 1x                │
│  tcmalloc（内存池） │ 50ms             │ 20x               │
│  jemalloc（内存池） │ 60ms             │ 16x               │
└─────────────────────────────────────────────────────────────┘
```

#### 为什么需要三级分层？

```
单层内存池的问题：
┌─────────────────────────────────────────────────────────────┐
│  一级内存池（只有ThreadCache）：                            │
│  - 无锁分配 → 极快                                        │
│  - 但是线程退出时内存无法回收                              │
│  → 内存只增不减，最终耗尽                                 │
└─────────────────────────────────────────────────────────────┘

三级分层的优势：
┌─────────────────────────────────────────────────────────────┐
│  ThreadCache（第一层）：                                    │
│  - thread_local，无锁                                       │
│  - 分配极快                                                │
│  - 线程退出时归还给CentralCache                            │
│                                                              │
│  CentralCache（第二层）：                                   │
│  - 所有线程共享                                            │
│  - 细粒度锁（按Size Class）                               │
│  - 空闲过多时归还给PageCache                               │
│                                                              │
│  PageCache（第三层）：                                      │
│  - 向系统申请/释放内存                                      │
│  - 管理物理页                                              │
│  - 合并相邻空闲页减少碎片                                  │
└─────────────────────────────────────────────────────────────┘
```

---

### 3.3.2 ThreadCache实现

#### 为什么需要ThreadCache？

ThreadCache是内存池的第一层，每个线程独立访问自己的缓存，完全无锁：

| 特性 | 说明 |
|-----|------|
| **零锁开销** | 同一线程内分配释放不需要任何锁 |
| **thread_local** | 每个线程有独立的缓存实例 |
| **Size Class** | 按大小分类管理，减少碎片 |
| **批量获取** | 从CentralCache批量获取，摊薄开销 |

#### 为什么需要Size Class（大小分类）？

```
不使用Size Class（浪费）：
申请100字节 → 系统分配4096字节（1页）→ 大量浪费

使用Size Class（节省）：
申请100字节 → 找到最接近的Size Class（如128B）→ 只用128B
申请256字节 → 从Span中切出256字节

Size Class设计（29个级别）：
8B, 16B, 32B, 48B, 64B, 96B, 128B, 192B, 256B,
384B, 512B, 768B, 1024B, 1536B, 2048B, 3072B, 4096B,
6144B, 8192B, 12288B, 16384B, 24576B, 32768B, 49152B,
65536B, 98304B, 131072B, 196608B, 262144B
```

#### 实现要点

```cpp
// ThreadCache包含：
// - free_list_：每个Size Class对应一个空闲链表
// - allocate(size_class)：从空闲链表分配
// - deallocate(ptr, size_class)：归还到空闲链表
// - fetch_from_central(size_class)：从CentralCache获取更多对象
// - return_to_central(size_class)：归还空闲对象给CentralCache

class ThreadCache {
private:
    std::vector<FreeList> free_lists_;  // 每个Size Class一个空闲链表
    static thread_local ThreadCache* instance_;

public:
    static ThreadCache* get_instance() {
        if (!instance_) {
            instance_ = new ThreadCache();
        }
        return instance_;
    }

    void* allocate(size_t size_class);
    void deallocate(void* ptr, size_t size_class);
};
```

#### 验收标准

- ThreadCache分配和释放无锁
- 多线程环境下不会相互干扰
- 空闲对象能正确归还CentralCache

---

### 3.3.3 CentralCache实现

#### 为什么需要CentralCache？

CentralCache是内存池的中间层，连接ThreadCache和PageCache：

| 特性 | 说明 |
|-----|------|
| **跨线程共享** | 所有线程共享CentralCache |
| **细粒度锁** | 每个Size Class独立锁，减少锁竞争 |
| **批量分配** | 向PageCache申请大块，切分后分配给ThreadCache |
| **高效回收** | 回收ThreadCache归还的对象 |

#### 细粒度锁的优势

```
全局锁方案（不好）：
┌─────────┐
│ 全局锁  │  ← 线程A访问时，线程B、C都要等待
└─────────┘

细粒度锁方案（好）：
┌─────────┬─────────┬─────────┐
│ Size0锁 │ Size1锁 │ Size2锁 │ ...
└─────────┴─────────┴─────────┘
线程A访问Size0，线程B访问Size1 → 并行执行
```

#### Span结构

```
Span包含：
- page_id_：起始页号
- num_pages_：页数量
- size_class_：属于哪个Size Class
- objects_free_：空闲对象数量
- next/prev：链表指针
```

#### 实现要点

```cpp
class CentralCache {
private:
    std::vector<SpanList> spans_;     // 每个Size Class的Span链表
    std::vector<std::mutex> locks_;  // 每个链表的锁

public:
    void* allocate(size_t size_class);
    void deallocate(void* ptr, size_t size_class);
    Span* fetch_from_pagecache(size_t size_class);
};
```

#### 验收标准

- CentralCache能正确管理多个Span链表
- 细粒度锁能正确保护各个链表
- 并发分配性能优于全局锁

---

### 3.3.4 PageCache实现

#### 为什么需要PageCache？

PageCache是内存池的最底层，直接与操作系统交互：

| 特性 | 说明 |
|-----|------|
| **页为单位管理** | 4KB为基本单位，与操作系统内存管理对齐 |
| **Span抽象** | 将连续内存页打包成Span统一管理 |
| **内存合并** | 释放时合并相邻Span，减少碎片 |
| **按需申请** | 根据需求向系统申请或释放内存 |

#### Span合并示意图

```
合并前：
┌─────┬─────┬─────┬─────┐
│Span1│Span2│Span3│Span4│  4个空闲Span
└─────┴─────┴─────┴─────┘

释放Span2时，发现Span1和Span3也空闲：
┌─────┬─────┬─────┬─────┐
│Span1│Span2│Span3│Span4│
└─────┴─────┴─────┴─────┘
  ↑ 相邻   ↑ 相邻

合并后：
┌─────────────────┬─────┐
│    大Span       │Span4│  合并成2个更大的Span
└─────────────────┴─────┘
```

#### 实现要点

```cpp
class PageCache {
private:
    std::map<uint64_t, Span*> span_map_;    // 页号到Span的映射
    std::list<Span*> free_span_list_;     // 空闲Span链表
    std::mutex mutex_;                     // 全局锁

public:
    Span* allocate_span(size_t num_pages);
    void free_span(Span* span);
    void coalesce_span(Span* span);  // 合并相邻Span
};
```

#### 验收标准

- PageCache能正确管理内存页
- 能正确向系统申请和释放内存
- 合并相邻Span能减少内存碎片

---

### 3.3.5 内存池整合

#### 三级协作流程

```
分配流程：
┌─────────────────────────────────────────────────────────────┐
│  ThreadCache.allocate(size_class)                           │
│     ↓ 有空闲对象？直接返回                                   │
│     ↓ 无空闲，从CentralCache获取                            │
│                                                              │
│  CentralCache.allocate(size_class)                          │
│     ↓ 有空闲Span？切分成对象返回                            │
│     ↓ 无空闲Span，从PageCache获取                           │
│                                                              │
│  PageCache.allocate(num_pages)                             │
│     ↓ 有足够大的Span？返回                                  │
│     ↓ 向系统申请新内存                                      │
└─────────────────────────────────────────────────────────────┘

归还流程：
┌─────────────────────────────────────────────────────────────┐
│  ThreadCache.deallocate(obj, size_class)                    │
│     ↓ 空闲对象过多（超过阈值）？                            │
│     ↓ 批量归还给CentralCache                                 │
│                                                              │
│  CentralCache.deallocate(obj, size_class)                   │
│     ↓ Span全部空闲且过多？                                  │
│     ↓ 归还给PageCache                                        │
│                                                              │
│  PageCache.free_span(span)                                 │
│     ↓ 合并相邻空闲Span                                       │
│     ↓ 内存返还系统（如果足够多）                            │
└─────────────────────────────────────────────────────────────┘
```

#### 为什么大内存直接用malloc？

- 超过256KB的内存不适合内存池管理
- 大内存请求不频繁，不需要优化
- 直接用malloc更简单，避免复杂化

#### 验收标准

- 内存分配和释放在单线程和多线程环境下都正确
- 内存池性能优于原生malloc/free（多线程环境下至少快3倍）
- 内存碎片率低于10%
- 大内存分配（>256KB）正常工作

---

## 3.4 章节3：线程池实现

### 3.4.1 为什么需要线程池？

#### 问题分析

```
每任务一线程的问题：
┌─────────────────────────────────────────────────────────────┐
│  // 收到1000个并发请求                                      │
│  for (int i = 0; i < 1000; i++) {                        │
│      std::thread t(handle_request, requests[i]);          │
│      t.detach();                                          │
│  }                                                        │
│                                                              │
│  问题：                                                    │
│  - 1000个线程创建/销毁开销巨大                             │
│  - 每个线程占用1MB+栈内存 → 1000线程占用1GB+              │
│  - 上下文切换开销大                                        │
│  - 线程总数受限于系统限制                                  │
└─────────────────────────────────────────────────────────────┘
```

#### 线程池的优势

| 方案 | 线程创建销毁 | 上下文切换 | 内存消耗 |
|-----|-------------|-----------|---------|
| 每任务一线程 | 高（每次创建） | 多 | 大（线程栈） |
| 线程池 | 低（复用） | 少 | 小（固定数量） |

#### 生产者-消费者模型

```
┌─────────────┐         ┌─────────────────┐         ┌─────────────┐
│ 生产者线程  │ 任务 →  │   任务队列      │ 任务 →  │ 消费者线程  │
│ (MainReactor)│         │ (ThreadSafeQueue)│         │ (Worker)    │
└─────────────┘         └─────────────────┘         └─────────────┘

提交任务：主线程将任务放入队列
取出任务：Worker线程从队列取出任务执行
```

---

### 3.4.2 实现要点

```cpp
class ThreadPool {
private:
    std::vector<std::thread> workers_;   // 工作线程列表
    BlockingQueue<Task> task_queue_;     // 任务队列
    std::atomic_bool running_;            // 运行状态
    size_t thread_num_;                   // 线程数

public:
    // 启动线程池
    void start(size_t num_threads) {
        thread_num_ = num_threads;
        running_ = true;
        for (size_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // 提交任务，返回future
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        task_queue_.enqueue([task]() { (*task)(); });
        return task->get_future();
    }

    // 优雅关闭
    void shutdown() {
        running_ = false;
        task_queue_.shutdown();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }
};
```

#### 为什么要返回std::future？

```cpp
// 提交任务并获取结果
auto future = thread_pool.submit([] {
    return expensive_computation();
});

// 主线程可以做其他事情
do_something_else();

// 获取任务结果（如果还没完成会等待）
auto result = future.get();
```

#### 验收标准

- 所有提交的任务都能正确执行
- 线程池吞吐量达到预期（>100,000任务/秒）
- 优雅关闭功能正常，不会丢失任务
- 异常任务不会导致线程池崩溃
- std::future能正确返回任务结果

---

## 3.5 章节4：网络模型升级

### 3.5.1 问题分析

```
单Reactor的瓶颈：
┌─────────────────────────────────────────────────────────────┐
│  EventLoop (单线程)                                         │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                  1000个连接                          │   │
│  │   连接1 ──► 连接2 ──► 连接3 ──► ... ──► 连接1000   │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                              │
│  问题：                                                    │
│  - 只能利用1个CPU核心                                      │
│  - 处理能力受限于单核性能                                   │
│  - 8核CPU服务器，只用了1核，其他7核闲着                   │
└─────────────────────────────────────────────────────────────┘

解决方案：MainSubReactor（多线程Reactor）
┌─────────────────────────────────────────────────────────────┐
│  MainReactor (主线程)                                       │
│  ─────────────────────────────────────────────────────     │
│  只负责accept，接受新连接                                    │
│                                                              │
│  SubReactor 1 (子线程1) ──► 处理500个连接                 │
│  SubReactor 2 (子线程2) ──► 处理500个连接                 │
│  SubReactor 3 (子线程3) ──► 处理500个连接                 │
│  SubReactor 4 (子线程4) ──► 处理500个连接                 │
│                                                              │
│  优势：                                                    │
│  - 利用多核CPU                                            │
│  - 每个SubReactor独立处理                                   │
│  - 一个SubReactor崩溃不影响其他                            │
└─────────────────────────────────────────────────────────────┘
```

---

### 3.5.2 MainSubReactor架构

```
                        ┌─────────────────────────┐
                        │     MainReactor          │
                        │   (主线程/主EventLoop)    │
                        │  监听socket              │
                        └───────────┬─────────────┘
                                    │ accept()
                                    ▼
                    ┌───────────────┴───────────────┐
                    │         连接分配               │
                    │    轮询（round-robin）         │
                    └───────────────┬───────────────┘
           ┌────────────────────────┼────────────────────────┐
           ▼                        ▼                        ▼
┌───────────────────┐    ┌───────────────────┐    ┌───────────────────┐
│   SubReactor 1    │    │   SubReactor 2    │    │   SubReactor N    │
│   (子线程1)       │    │   (子线程2)       │    │   (子线程N)       │
│   EventLoop       │    │   EventLoop       │    │   EventLoop       │
│   Connection列表  │    │   Connection列表  │    │   Connection列表  │
│   处理1000连接    │    │   处理1000连接    │    │   处理1000连接    │
└───────────────────┘    └───────────────────┘    └───────────────────┘
```

---

### 3.5.3 为什么SubReactor崩溃不影响其他？

- 每个SubReactor是独立线程
- 各自管理自己的连接
- 一个SubReactor崩溃只影响它管理的连接

---

### 3.5.4 实现要点

```cpp
class MainReactor {
private:
    EventLoop main_loop_;
    Socket listen_socket_;

public:
    void accept_connection();  // 接受新连接
};

class SubReactor {
private:
    std::thread thread_;
    EventLoop event_loop_;
    std::unordered_map<int, Connection> connections_;

public:
    void add_connection(int fd);
    void remove_connection(int fd);
    void run();  // 线程入口函数
};

class SubReactorPool {
private:
    std::vector<SubReactor> sub_reactors_;
    std::atomic<size_t> next_index_;  // 轮询索引

public:
    SubReactor& get_next() {
        return sub_reactors_[next_index_++ % sub_reactors_.size()];
    }
};
```

#### 验收标准

- 支持1000个并发连接
- 连接能正确分配到各个SubReactor
- 负载均衡（连接均匀分布）
- 某一SubReactor崩溃不影响其他SubReactor

---

## 3.6 章节5：缓存核心实现

### 3.6.1 分段锁哈希表

#### 为什么全局锁成为瓶颈？

```
全局锁方案（不好）：
┌─────────────────────────────┐
│         全局锁               │ ← 所有线程都要抢这一把锁
│  ┌───────────────────────┐  │
│  │    全局哈希表         │  │
│  │  key1 → value1        │  │
│  │  key2 → value2        │  │
│  │  ...                   │  │
│  └───────────────────────┘  │
└─────────────────────────────┘

性能对比：
┌─────────────────────────────────────────────────────────────┐
│  方案              │ 并发读 QPS  │ 并发写 QPS  │ 锁竞争   │
│  ─────────────────────────────────────────────────          │
│  全局互斥锁        │ 100万/秒     │ 10万/秒     │ 严重     │
│  分段锁（16分片）  │ 800万/秒     │ 80万/秒     │ 轻微     │
└─────────────────────────────────────────────────────────────┘
```

#### 分段锁原理

```
分段锁方案（好）：
┌────────┬────────┬────────┬────────┐
│分片0锁 │分片1锁 │分片2锁 │分片3锁 │ ...
└────────┴────────┴────────┴────────┘
┌──────┐│┌──────┐│┌──────┐│┌──────┐│
│分片0  │││分片1  │││分片2  │││分片3  ││
│key0   │││key1   │││key2   │││key3   ││
│key4   │││key5   │││key6   │││key7   ││
└──────┘│└──────┘│└──────┘│└──────┘│
└────────┴────────┴────────┴────────┘
线程A访问分片0，线程B访问分片1 → 完全并行
```

#### 分片数量选择

- 分片数太少 → 锁竞争仍然明显
- 分片数太多 → 每个分片数据太少，缓存局部性差
- 建议：CPU核心数 × 2，如8核CPU用16分片

#### 实现要点

```cpp
class HashTable {
private:
    struct Shard {
        std::unordered_map<std::string, std::string> map_;
        RWLock rwlock_;  // 每个分片独立的读写锁
    };

    std::vector<Shard> shards_;
    size_t num_shards_;

    size_t get_shard_index(const std::string& key) {
        return std::hash<std::string>{}(key) % num_shards_;
    }

public:
    explicit HashTable(size_t num_shards = 16) : shards_(num_shards) {}

    std::string get(const std::string& key) {
        auto& shard = shards_[get_shard_index(key)];
        ReadLockGuard lock(shard.rwlock_);
        auto it = shard.map_.find(key);
        return (it != shard.map_.end()) ? it->second : "";
    }

    void set(const std::string& key, const std::string& value) {
        auto& shard = shards_[get_shard_index(key)];
        WriteLockGuard lock(shard.rwlock_);
        shard.map_[key] = value;
    }
};
```

#### 验收标准

- 所有基本操作在单线程和多线程环境下都正确
- 并发读写性能明显优于全局锁哈希表（至少快5倍）
- 内存池能正确管理哈希表节点内存

---

### 3.6.2 过期字典和过期删除

#### 为什么需要过期机制？

缓存数据不应该永远存在，过期机制让数据能够自动清理：

| 需求 | 说明 |
|-----|------|
| **内存有限** | 数据永不过期会导致内存耗尽 |
| **业务需求** | 有些数据只需要短期存在（如验证码、临时token） |
| **TTL支持** | Redis核心功能之一 |

#### 两种删除策略互补

| 策略 | 触发时机 | 优点 | 缺点 |
|-----|---------|------|------|
| **惰性删除** | 访问键时 | 节省CPU，只删该删的 | 不访问的键永不删除 |
| **定期删除** | 定时任务 | 定期清理不访问的键 | 可能影响CPU |

#### 惰性删除流程

```
GET key1
  → 检查key1是否过期？
    → 没过期 → 返回value
    → 已过期 → 删除key1 → 返回null
```

#### 定期删除流程

```
每100ms执行一次：
  随机抽取20个键
    → 检查每个键是否过期？
      → 已过期 → 删除
    → 时间超过25ms → 停止（避免占用CPU）
```

#### EXPIRE/TTL/PERSIST命令

```
EXPIRE key 60      # 设置60秒后过期
TTL key            # 返回剩余生存时间（秒）
TTL key            # 返回-2（键不存在）或-1（永不过期）
PERSIST key        # 移除过期时间，变成永久键
```

#### 实现要点

```cpp
class ExpireDict {
private:
    std::unordered_map<std::string, int64_t> expire_map_;  // 键 → 过期时间(ms)

public:
    void set(const std::string& key, int64_t expire_time_ms);
    int64_t get(const std::string& key);
    void remove(const std::string& key);
    bool is_expired(const std::string& key);
    std::vector<std::string> get_expired_keys(int n);  // 随机获取n个可能过期的键
};
```

#### 验收标准

- 过期键能被正确删除
- EXPIRE、TTL、PERSIST命令工作正常
- 惰性删除在访问时触发
- 定期删除不会占用过多CPU
- 过期键删除后能释放内存

---

### 3.6.3 LRU缓存替换算法

#### 为什么需要缓存替换算法？

内存是有限的，当缓存数据占满所有内存时，需要淘汰一些数据：

| 需求 | 说明 |
|-----|------|
| **内存上限** | max_memory配置限制了最大内存使用 |
| **淘汰策略** | 决定哪些数据该被淘汰 |
| **热点优先** | 最近使用的数据更可能是热点 |

#### 为什么选择LRU？

- 原理简单：最近使用的，下次也更可能使用
- 实现相对简单：链表记录访问顺序
- 效果好：适合读多写多的场景

#### 近似LRU vs 真实LRU

```
真实LRU：
每次访问都要移动链表节点到头部
多线程下需要加锁，锁竞争严重

近似LRU（Redis采用）：
记录每个键的访问时间戳（精确到毫秒）
淘汰时随机选5个键，淘汰最老的
效果接近真实LRU，但性能好很多
```

#### LRU链表示意图

```
LRU链表（头部最新，尾部最旧）：

访问key1后：                    内存满，需要淘汰时：
┌────┬────┬────┬────┐          ┌────┬────┬────┬────┐
│key1│key3│key2│key4│          │key1│key3│key2│key4│
└────┴────┴────┴────┘          └────┴────┴────┴────┘
 ↑头                ↑尾                     ↓
                访问时移动到头          从尾部淘汰key4
```

#### 实现要点

```cpp
// EvictionPolicy基类
class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;
    virtual void on_access(const std::string& key) = 0;
    virtual bool should_evict() const = 0;
    virtual std::string evict() = 0;
    virtual void on_set(const std::string& key) = 0;
    virtual void on_del(const std::string& key) = 0;
};

// LRU策略
class LRUEvictionPolicy : public EvictionPolicy {
private:
    std::list<std::string> lru_list_;  // 头部最新，尾部最旧
    std::unordered_map<std::string, std::list<std::string>::iterator> pos_map_;

public:
    void on_access(const std::string& key) override;
    std::string evict() override;
};
```

#### 验收标准

- 内存使用能被控制在配置的上限以内
- 最久未访问的键优先被淘汰
- 近似LRU算法的准确性达到90%以上
- 淘汰键后内存能正确释放

---

## 3.7 章节6：命令增强

### 3.7.1 字符串命令增强

#### 为什么需要完整命令集？

骨架版本只实现了4个基础命令，完整命令集提供了更丰富的功能：

| 命令类型 | 命令 | 解决的问题 |
|--------|------|-----------|
| **原子计数** | INCR/DECR | 计数器并发安全，比GET+SET+PARSE更快 |
| **字符串操作** | APPEND/STRLEN | 在字符串末尾追加、获取长度 |
| **原子设置** | SETNX/SETEX | 键不存在时设置/带过期时间设置 |

#### INCR命令的原子性

```cpp
// 非原子方式（多线程不安全）
value = GET key           // 读取
value = value + 1         // 修改
SET key value             // 写回
// 如果两个线程同时执行，可能都读到0，都写成1

// INCR命令（原子操作）
INCR key  // 由内存引擎保证原子性，不会出现上述问题
```

#### SETNX的应用场景

```
分布式锁：
SETNX lock_key unique_id  # 只有不存在时才能设置成功
# 成功 → 获取锁
# 失败 → 锁已被其他客户端持有
```

#### 命令列表

```
GET key
SET key value
SETNX key value
SETEX key seconds value
DEL key [key ...]
EXISTS key [key ...]
INCR key
DECR key
INCRBY key increment
DECRBY key decrement
APPEND key value
STRLEN key
```

#### 验收标准

- 所有字符串命令都能正确执行
- 与redis-cli兼容
- 边界情况处理正确（空字符串、大字符串、整数溢出返回错误）

---

## 3.8 基础版本验收清单

| 模块 | 功能 | 验收标准 |
|------|------|----------|
| 日志系统 | 文件输出、滚动 | 能按大小和时间滚动 |
| 配置系统 | 热加载 | 修改配置后自动生效 |
| 信号处理 | SIGSEGV堆栈 | 能打印堆栈信息 |
| 锁机制 | Mutex/SpinLock/RWLock | 多线程安全 |
| 内存池 | 三级分层 | 性能优于malloc 3倍+ |
| 线程池 | 固定任务池 | 吞吐量>10万/秒 |
| 网络模型 | MainSubReactor | 支持1000并发 |
| 哈希表 | 分段锁 | 并发性能提升5倍+ |
| 过期删除 | 惰性+定期 | EXPIRE/TTL正常 |
| LRU算法 | 近似LRU | 准确性>90% |
| 字符串命令 | 完整命令集 | redis-cli兼容 |

---

## 3.9 章节总结

### 基础版本学到了什么

```
核心架构：

┌─────────────────────────────────────────────────────────────┐
│                    MainSubReactor                           │
│  ┌─────────────────────────────────────────────────────┐  │
│  │ MainReactor → SubReactor1 → SubReactor2 → ...       │  │
│  │ (accept)     (EventLoop)  (EventLoop)                │  │
│  └─────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                    Memory Pool (三级分层)                    │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐             │
│  │ThreadCache│───▶│Central   │───▶│Page      │             │
│  │(TLS无锁) │◀───│Cache     │◀───│Cache     │             │
│  └──────────┘    │(细粒度锁)│    │(全局锁)  │             │
│                   └──────────┘    └──────────┘             │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                    Cache Core                                │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐             │
│  │ 分段锁   │    │ 过期字典 │    │ LRU算法  │             │
│  │ 哈希表   │    │ (TTL)    │    │          │             │
│  └──────────┘    └──────────┘    └──────────┘             │
└─────────────────────────────────────────────────────────────┘
```

### 基础版本的局限性

```
┌─────────────────────────────────────────────────────────────┐
│                    基础版本的不足                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. 只支持String类型                                        │
│     → 无法满足复杂业务场景（用户信息、排行榜、消息队列）      │
│                                                             │
│  2. 没有持久化                                              │
│     → 进程重启数据全部丢失                                  │
│                                                             │
│  3. 只有一个淘汰算法（LRU）                                 │
│     → 不同业务场景需要不同策略                              │
│                                                             │
│  这些问题将在 Version3 中解决！                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 下一步

基础版本完成后，我们将进入**增强版本（Version3）**，添加更多数据类型和持久化功能：
- Hash/List/Set/ZSet数据类型
- LFU/FIFO/Random缓存替换算法
- RDB快照持久化
- AOF追加日志持久化

[下一章：增强版本](./03_增强版本.md)
