# ConcurrentCache 基础版本（Version2）

## 版本概述

**目标**：在骨架版本基础上，完善基础工具，添加完整内存池、线程池和缓存核心功能。

基础版本是项目的核心部分，实现高性能缓存系统所需的所有基础设施。

**预计开发周期**：4-6周

**核心功能**：
- 日志文件输出、滚动、配置热加载
- 各种锁机制（Mutex、SpinLock、RWLock）
- 三级分层内存池（对标tcmalloc）
- 固定任务线程池
- MainSubReactor多线程网络模型
- 分段锁全局哈希表
- 过期键删除（惰性删除+定期删除）
- LRU缓存替换算法
- 完整的字符串命令集

---

## 组件作用说明

### 为什么要开发基础版本？

骨架版本只实现了"能跑"的基础功能，基础版本则实现了"跑得快、跑得稳"的关键能力：

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
| **完整字符串命令** | 只有基础GET/SET | 支持计数器、过期等丰富操作 |

---

## 架构图

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

## 章节1：基础工具增强（第1-2周）

### 任务1.1：日志系统完善

**目标**：完善日志系统，支持文件输出和滚动

**为什么需要这个组件？**

骨架版本的日志只输出到控制台，在生产环境中无法保存日志文件。完善的日志系统解决了以下问题：

1. **日志持久化**：程序重启后可以通过日志追溯历史问题
2. **文件滚动**：防止日志文件无限增长占用磁盘空间
3. **异步写入**：日志写入不阻塞主线程，不影响性能
4. **多级别输出**：可以控制不同模块的日志详细程度

**日志滚动的必要性**：
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

**实现要点**：
1. 支持控制台输出和文件输出
2. 支持日志文件滚动（按大小100MB和按天）
3. 提供不同模块的logger（base、network、cache等）
4. 日志格式包含：时间戳（毫秒级）、线程ID、日志级别、模块名、消息
5. 支持异步日志写入
6. 可配置日志级别

**类设计**：
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

**验收标准**：
- 日志能同时输出到控制台和文件
- 日志文件能按配置自动滚动
- 多线程环境下日志输出不混乱
- 异步写入不影响主线程性能

---

### 任务1.2：配置系统完善

**目标**：完善配置系统，支持热加载

**为什么需要这个组件？**

配置热加载让运维人员可以在不重启服务的情况下调整配置，大大提高了系统的灵活性：

1. **不停机调整**：修改配置后自动生效，无需停止服务
2. **观察者模式**：配置变更时自动通知关心该配置的模块
3. **配置验证**：确保新配置值合法，避免程序异常
4. **分类管理**：不同类型的配置分类管理，更清晰

**热加载的使用场景**：
```
# 场景1：动态调整日志级别
# 发现问题后，把日志级别从info改成debug，获取更多信息
# 修改concurrentcache.conf: log_level = debug
# 配置系统检测到变更，自动通知logger模块

# 场景2：调整内存上限
# 发现内存使用接近上限，需要调整
# 修改concurrentcache.conf: max_memory = 2147483648  # 2GB
# 配置系统通知cache模块，缓存模块重新计算淘汰策略
```

**实现要点**：
1. 支持更多数据类型：int、string、bool、double
2. 支持热加载配置文件
3. 配置项分类管理（网络、线程、缓存、持久化等分类）
4. 提供配置变更回调机制
5. 配置验证（类型检查、范围检查）

**主要配置项**：
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

**类设计**：
```cpp
// 配置系统包含：
// - Config类：单例，配置管理
// - 配置观察者模式：配置变更时通知订阅者
// - 配置验证器：检查配置值是否合法
```

**验收标准**：
- 能正确读取所有配置项
- 修改配置文件后能自动重新加载
- 未配置的项能使用默认值
- 配置热加载时不会影响运行中的服务

---

### 任务1.3：信号处理完善

**目标**：完善信号处理，支持更多信号

**为什么需要这个组件？**

完善的信号处理让程序在异常情况下能够输出关键调试信息：

1. **SIGSEGV处理**：程序崩溃时打印堆栈信息，帮助定位问题
2. **SIGPIPE忽略**：防止向已关闭连接写入导致程序崩溃
3. **优雅退出**：处理退出信号时等待任务完成再退出
4. **可扩展性**：提供信号注册接口，方便添加新信号处理

**SIGSEGV堆栈信息的价值**：
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

**实现要点**：
1. 处理SIGINT、SIGTERM：优雅退出（等待当前请求处理完成）
2. 处理SIGSEGV：打印堆栈信息并退出
3. 处理SIGPIPE：忽略，防止写已关闭连接崩溃
4. 提供信号注册接口（SignalHandler类）
5. 支持自定义信号处理函数

**类设计**：
```cpp
// 信号系统包含：
// - SignalHandler类：信号处理管理
// - register_handler(sig, callback)：注册信号处理函数
// - StackTrace类：堆栈信息打印
```

**验收标准**：
- 程序收到SIGINT/SIGTERM信号后能优雅退出
- 收到SIGSEGV信号后能打印堆栈信息
- SIGPIPE不会导致程序崩溃

---

### 任务1.4：锁机制实现

**目标**：实现各种锁机制，供其他模块使用

**为什么需要这个组件？**

多线程并发访问共享资源时，需要锁来保证数据一致性。不同场景需要不同的锁：

| 锁类型 | 适用场景 | 特点 |
|-------|---------|------|
| **Mutex** | 一般互斥 | 简单，易死锁 |
| **SpinLock** | 短临界区 | 忙等，不释放CPU |
| **RWLock** | 读多写少 | 读并行，写独占 |

**Mutex vs SpinLock**：
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

**为什么需要RAII锁守卫？**
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

**实现要点**：
1. Mutex：基本互斥锁（基于std::mutex）
2. SpinLock：自旋锁（基于atomic实现，忙等）
3. RWLock：读写锁（基于std::shared_mutex）
4. LockGuard：RAII风格互斥锁守卫
5. ReadLockGuard：RAII风格读锁守卫
6. WriteLockGuard：RAII风格写锁守卫

**类设计**：
```cpp
// 锁系统包含：
// - Mutex类：互斥锁
// - SpinLock类：自旋锁
// - RWLock类：读写锁
// - LockGuard类模板：互斥锁守卫
// - ReadLockGuard类模板：读锁守卫
// - WriteLockGuard类模板：写锁守卫

// 使用示例：
// {
//     LockGuard<Mutex> guard(mutex_);
//     // 临界区
// }
```

**验收标准**：
- 所有锁机制在多线程环境下都正确
- 能正确保护共享资源
- 读写锁支持多个读并发、一个写独占
- 自旋锁在短临界区性能优于互斥锁

---

## 章节2：内存池实现（第3-4周）

### 任务2.1：ThreadCache实现

**目标**：实现线程本地缓存层

**为什么需要这个组件？**

ThreadCache是内存池的第一层，每个线程独立访问自己的缓存，完全无锁：

1. **零锁开销**：同一线程内分配释放不需要任何锁
2. **thread_local**：每个线程有独立的缓存实例
3. **Size Class**：按大小分类管理，减少碎片
4. **批量获取**：从CentralCache批量获取，摊薄开销

**为什么需要Size Class（大小分类）？**
```
不使用Size Class：
申请100字节 → 系统分配4096字节（1页），大量浪费

使用Size Class：
申请100字节 → 找到最接近的Size Class（如128B）
申请128字节 → 从Span中切出128字节，浪费28字节
申请256字节 → 从Span中切出256字节
```

**实现要点**：
1. 使用线程本地存储（thread_local）
2. 每个线程独立一个ThreadCache实例
3. 按大小分类管理内存对象（Size Class）
4. 无锁分配释放（ThreadCache内操作无需加锁）
5. 线程退出时归还所有空闲对象到CentralCache

**Size Class设计**：
```
8B, 16B, 32B, 48B, 64B, 96B, 128B, 192B, 256B,
384B, 512B, 768B, 1024B, 1536B, 2048B, 3072B, 4096B,
6144B, 8192B, 12288B, 16384B, 24576B, 32768B, 49152B,
65536B, 98304B, 131072B, 196608B, 262144B
```

**类设计**：
```cpp
// ThreadCache包含：
// - free_list_：每个Size Class对应一个空闲链表
// - allocate(size_class)：从空闲链表分配
// - deallocate(ptr, size_class)：归还到空闲链表
// - fetch_from_central(size_class)：从CentralCache获取更多对象
// - return_to_central(size_class)：归还空闲对象给CentralCache
```

**验收标准**：
- ThreadCache分配和释放无锁
- 多线程环境下不会相互干扰
- 空闲对象能正确归还CentralCache

---

### 任务2.2：CentralCache实现

**目标**：实现中心缓存层

**为什么需要这个组件？**

CentralCache是内存池的中间层，连接ThreadCache和PageCache：

1. **跨线程共享**：所有线程共享CentralCache
2. **细粒度锁**：每个Size Class独立锁，减少锁竞争
3. **批量分配**：向PageCache申请大块，切分后分配给ThreadCache
4. **高效回收**：回收ThreadCache归还的对象

**细粒度锁的优势**：
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

**实现要点**：
1. 每个Size Class对应一个Span链表
2. 使用细粒度锁（每个Span链表一把锁）
3. 从PageCache申请内存页
4. 向ThreadCache分配内存对象
5. 回收ThreadCache归还的内存

**Span结构**：
```
Span包含：
- page_id_：起始页号
- num_pages_：页数量
- size_class_：属于哪个Size Class
- objects_free_：空闲对象数量
- next/prev：链表指针
```

**类设计**：
```cpp
// CentralCache包含：
// - spans_[size_class]：每个Size Class的Span链表
// - locks_[size_class]：每个链表的锁
// - allocate(size_class)：分配一个对象
// - deallocate(ptr, size_class)：回收一个对象
// - fetch_from_pagecache(size_class)：从PageCache获取新Span
```

**验收标准**：
- CentralCache能正确管理多个Span链表
- 细粒度锁能正确保护各个链表
- 并发分配性能优于全局锁

---

### 任务2.3：PageCache实现

**目标**：实现页缓存层

**为什么需要这个组件？**

PageCache是内存池的最底层，直接与操作系统交互：

1. **页为单位管理**：4KB为基本单位，与操作系统内存管理对齐
2. **Span抽象**：将连续内存页打包成Span统一管理
3. **内存合并**：释放时合并相邻Span，减少碎片
4. **按需申请**：根据需求向系统申请或释放内存

**Span合并示意图**：
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

**实现要点**：
1. 以4KB页为单位管理内存
2. 管理Span结构（内存页的抽象）
3. 向系统申请和释放内存（mmap/brk）
4. 合并相邻的空闲Span（减少碎片）

**类设计**：
```cpp
// PageCache包含：
// - span_map_：页号到Span的映射
// - free_span_list_：空闲Span链表
// - mutex_：全局锁
// - allocate_span(num_pages)：分配N个连续页
// - free_span(span)：释放Span
// - coalesce_span(span)：合并相邻Span
```

**验收标准**：
- PageCache能正确管理内存页
- 能正确向系统申请和释放内存
- 合并相邻Span能减少内存碎片

---

### 任务2.4：内存池整合

**目标**：整合三级缓存，实现完整的内存池

**为什么需要三级分层？**

三级分层的设计是为了在性能和内存利用率之间取得平衡：

| 层级 | 访问速度 | 锁竞争 | 管理单位 |
|-----|---------|-------|---------|
| ThreadCache | 最快 | 无锁 | 对象（按Size Class） |
| CentralCache | 快 | 细粒度锁 | Span（按Size Class切分） |
| PageCache | 慢 | 全局锁 | 页（4KB） |

**三级协作示意图**：
```
ThreadCache层（thread_local，无锁）：
┌──────────────────────────────────────────────────────┐
│ 线程A的ThreadCache  │  线程B的ThreadCache          │
│ free_list_[0]: ●●●● │  free_list_[0]: ●●●●        │
│ free_list_[1]: ●●    │  free_list_[1]: ●●●●        │
│ ...                  │  ...                         │
└──────────────────────────────────────────────────────┘
         ↓ 获取更多                    ↓ 获取更多
┌──────────────────────────────────────────────────────┐
│              CentralCache层（细粒度锁）                │
│  spans_[0]: [Span]←───────→[Span]                  │
│  spans_[1]: [Span]                                 │
│  ...                                                │
└──────────────────────────────────────────────────────┘
         ↓ 获取更多Span                    ↓ 释放Span
┌──────────────────────────────────────────────────────┐
│                  PageCache层（全局锁）                │
│  free_span_list_: [Span]←──→[Span]←──→[Span]     │
│  span_map_: 页号 → Span                              │
└──────────────────────────────────────────────────────┘
```

**为什么大内存直接用malloc？**
- 超过256KB的内存不适合内存池管理
- 大内存请求不频繁，不需要优化
- 直接用malloc更简单，避免复杂化

**实现要点**：
1. ThreadCache向CentralCache申请内存
2. CentralCache向PageCache申请页
3. ThreadCache空闲对象过多时归还给CentralCache
4. CentralCache空闲Span过多时归还给PageCache
5. 实现大内存分配（>256KB）直接使用malloc
6. 提供全局分配接口：allocate()、deallocate()

**工作流程**：
```
分配流程：
1. ThreadCache.allocate(size_class)
   → 有空闲对象？直接返回
   → 无空闲，从CentralCache获取

2. CentralCache.allocate(size_class)
   → 有空闲Span？切分成对象返回
   → 无空闲Span，从PageCache获取

3. PageCache.allocate(num_pages)
   → 有足够大的Span？返回
   → 向系统申请新内存

归还流程：
1. ThreadCache.deallocate(obj, size_class)
   → 空闲对象过多（超过阈值）？
     → 批量归还给CentralCache

2. CentralCache.deallocate(obj, size_class)
   → Span全部空闲且过多？
     → 归还给PageCache

3. PageCache.free_span(span)
   → 合并相邻空闲Span
   → 内存返还系统（如果足够多）
```

**验收标准**：
- 内存分配和释放在单线程和多线程环境下都正确
- 内存池性能优于原生malloc/free（多线程环境下至少快3倍）
- 内存碎片率低于10%
- 大内存分配（>256KB）正常工作

---

## 章节3：线程池实现（第5周）

### 任务3.1：固定任务线程池

**目标**：实现高性能的固定任务线程池

**为什么需要这个组件？**

线程池避免了为每个任务创建/销毁线程的开销：

| 方案 | 线程创建销毁 | 上下文切换 | 内存消耗 |
|-----|-------------|-----------|---------|
| 每任务一线程 | 高（每次创建） | 多 | 大（线程栈） |
| 线程池 | 低（复用） | 少 | 小（固定数量） |

**生产者-消费者模型**：
```
┌─────────────┐         ┌─────────────────┐         ┌─────────────┐
│ 生产者线程  │ 任务 →  │   任务队列      │ 任务 →  │ 消费者线程  │
│ (MainReactor)│         │ (ThreadSafeQueue)│         │ (Worker)    │
└─────────────┘         └─────────────────┘         └─────────────┘

提交任务：主线程将任务放入队列
取出任务：Worker线程从队列取出任务执行
```

**为什么要返回std::future？**
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

**实现要点**：
1. 采用生产者-消费者模型
2. 固定线程数（可通过配置设置，默认CPU核心数）
3. 线程安全的任务队列（支持阻塞和非阻塞入队）
4. 支持提交任意函数对象和参数，返回std::future
5. 支持优雅关闭（等待所有任务执行完毕后退出）
6. 支持线程池状态查询（运行中/关闭中）
7. 支持任务优先级（可选）

**类设计**：
```cpp
// ThreadPool包含：
// - workers_：工作线程列表
// - task_queue_：任务队列
// - mutex_：互斥锁
// - condition_：条件变量
// - running_：运行状态
// - shutdown()：优雅关闭

// 公共接口：
// - start(num_threads)：启动线程池
// - submit(func, args...)：提交任务，返回future
// - shutdown()：优雅关闭
// - wait_for_idle()：等待所有任务完成
```

**任务队列设计**：
```cpp
// TaskQueue包含：
// - queue_：任务队列
// - mutex_：互斥锁
// - not_empty_：非空条件变量
// - not_full_：非满条件变量（阻塞模式）
// - enqueue(task, blocking)：入队
// - dequeue()：出队
```

**验收标准**：
- 所有提交的任务都能正确执行
- 线程池吞吐量达到预期（>100,000任务/秒）
- 优雅关闭功能正常，不会丢失任务
- 异常任务不会导致线程池崩溃
- std::future能正确返回任务结果

---

## 章节4：网络模型升级（第6周）

### 任务4.1：MainSubReactor实现

**目标**：升级网络模型为多线程Reactor

**为什么需要这个组件？**

单Reactor只能利用一个CPU核心，处理能力有限。MainSubReactor让多个SubReactor并行处理：

| 方案 | CPU利用 | 处理能力 | 复杂度 |
|-----|-------|---------|-------|
| 单Reactor | 单核 | 10万连接/秒 | 简单 |
| MainSubReactor | 多核 | 100万连接/秒 | 中等 |

**MainSubReactor架构**：
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

**为什么SubReactor崩溃不影响其他？**
- 每个SubReactor是独立线程
- 各自管理自己的连接
- 一个SubReactor崩溃只影响它管理的连接

**实现要点**：
1. MainReactor：负责监听listenfd，接受连接
2. SubReactor池：每个SubReactor对应一个线程和一个EventLoop
3. 连接分配：轮询算法将新连接均匀分配给各个SubReactor
4. SubReactor数量可配置
5. 线程安全的连接管理

**架构设计**：
```
MainReactor（主线程）
    ↓
接受连接 → 轮询分配 → SubReactor1（子线程1）
                         ↓
                    EventLoop
                    ↓
              Connection1, Connection2, ...

                         SubReactor2（子线程2）
                         ↓
                    EventLoop
                    ↓
              Connection3, Connection4, ...
```

**类设计**：
```cpp
// MainReactor包含：
// - event_loop_：主事件循环
// - listen_socket_：监听socket
// - accept_connection()：接受连接

// SubReactorPool包含：
// - sub_reactors_：SubReactor列表
// - next_index_：轮询索引
// - get_next()：获取下一个SubReactor

// SubReactor包含：
// - thread_：工作线程
// - event_loop_：事件循环
// - connections_：连接映射
```

**验收标准**：
- 支持1000个并发连接
- 连接能正确分配到各个SubReactor
- 负载均衡（连接均匀分布）
- 某一SubReactor崩溃不影响其他SubReactor

---

## 章节5：缓存核心实现（第7-8周）

### 任务5.1：分段锁哈希表

**目标**：实现高性能的分段锁全局哈希表

**为什么需要这个组件？**

全局锁在多线程高并发场景下成为性能瓶颈：

| 方案 | 并发读 | 并发写 | 锁竞争 |
|-----|-------|-------|-------|
| 全局互斥锁 | 100万/秒 | 10万/秒 | 严重 |
| 分段锁（16分片） | 800万/秒 | 80万/秒 | 轻微 |

**分段锁原理**：
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

分段锁方案（好）：
┌────────┬────────┬────────┬────────┐
│分片0锁 │分片1锁 │分片2锁 │分片3锁 │ ...
│┌──────┐│┌──────┐│┌──────┐│┌──────┐│
││分片0  │││分片1  │││分片2  │││分片3  ││
││key0   │││key1   │││key2   │││key3   ││
││key4   │││key5   │││key6   │││key7   ││
│└──────┘│└──────┘│└──────┘│└──────┘│
└────────┴────────┴────────┴────────┘
线程A访问分片0，线程B访问分片1 → 完全并行
```

**分片数量选择**：
- 分片数太少 → 锁竞争仍然明显
- 分片数太多 → 每个分片数据太少，缓存局部性差
- 建议：CPU核心数 × 2，如8核CPU用16分片

**实现要点**：
1. 将哈希表划分为多个独立的分片（默认16个，可配置）
2. 每个分片有自己的读写锁（RWLock）
3. 每个分片使用std::unordered_map存储键值对
4. 键的哈希值对分片数取模，确定键属于哪个分片
5. 实现基本操作：get、set、del、exists
6. 使用内存池分配哈希表节点内存

**类设计**：
```cpp
// HashTable包含：
// - shards_：分片列表
// - num_shards_：分片数量

// Shard包含：
// - map_：unordered_map
// - rwlock_：读写锁
// - get(key)、set(key, value)、del(key)、exists(key)

class HashTable {
public:
    explicit HashTable(size_t num_shards = 16);
    std::string get(const std::string& key);
    void set(const std::string& key, const std::string& value);
    bool del(const std::string& key);
    bool exists(const std::string& key);
    size_t size();

private:
    size_t get_shard_index(const std::string& key);
    std::vector<Shard> shards_;
    size_t num_shards_;
};
```

**验收标准**：
- 所有基本操作在单线程和多线程环境下都正确
- 并发读写性能明显优于全局锁哈希表（至少快5倍）
- 内存池能正确管理哈希表节点内存

---

### 任务5.2：过期字典和过期删除

**目标**：实现键过期自动删除功能

**为什么需要这个组件？**

缓存数据不应该永远存在，过期机制让数据能够自动清理：

1. **内存有限**：数据永不过期会导致内存耗尽
2. **业务需求**：有些数据只需要短期存在（如验证码、临时token）
3. **TTL支持**：Redis核心功能之一

**两种删除策略互补**：
| 策略 | 触发时机 | 优点 | 缺点 |
|-----|---------|------|------|
| 惰性删除 | 访问键时 | 节省CPU，只删该删的 | 不访问的键永不删除 |
| 定期删除 | 定时任务 | 定期清理不访问的键 | 可能影响CPU |

**惰性删除流程**：
```
GET key1
  → 检查key1是否过期？
    → 没过期 → 返回value
    → 已过期 → 删除key1 → 返回null
```

**定期删除流程**：
```
每100ms执行一次：
  随机抽取20个键
    → 检查每个键是否过期？
      → 已过期 → 删除
    → 时间超过25ms → 停止（避免占用CPU）
```

**EXPIRE/TTL/PERSIST命令**：
```
EXPIRE key 60      # 设置60秒后过期
TTL key            # 返回剩余生存时间（秒）
TTL key            # 返回-2（键不存在）或-1（永不过期）
PERSIST key        # 移除过期时间，变成永久键
```

**实现要点**：
1. 每个分片维护一个过期字典，存储键到过期时间的映射
2. 过期时间使用毫秒级时间戳
3. 实现惰性删除：访问键时检查是否过期，如果过期则删除
4. 实现定期删除：每隔100ms，随机抽取20个键检查是否过期
5. 限制每次定期删除的时间不超过25ms，避免占用过多CPU
6. 实现EXPIRE、TTL、PERSIST命令

**过期字典设计**：
```cpp
// ExpireDict包含：
// - expire_map_：键到过期时间的映射
// - set(key, expire_time_ms)：设置过期时间
// - get(key)：获取过期时间
// - remove(key)：删除过期时间
// - is_expired(key)：检查是否过期
// - get_expired_keys(n)：随机获取n个可能过期的键
```

**验收标准**：
- 过期键能被正确删除
- EXPIRE、TTL、PERSIST命令工作正常
- 惰性删除在访问时触发
- 定期删除不会占用过多CPU
- 过期键删除后能释放内存

---

### 任务5.3：LRU缓存替换算法

**目标**：实现LRU缓存替换算法

**为什么需要这个组件？**

内存是有限的，当缓存数据占满所有内存时，需要淘汰一些数据：

1. **内存上限**：max_memory配置限制了最大内存使用
2. **淘汰策略**：决定哪些数据该被淘汰
3. **热点优先**：最近使用的数据更可能是热点

**为什么选择LRU？**
- 原理简单：最近使用的，下次也更可能使用
- 实现相对简单：链表记录访问顺序
- 效果好：适合读多写多的场景

**近似LRU vs 真实LRU**：
```
真实LRU：
每次访问都要移动链表节点到头部
多线程下需要加锁，锁竞争严重

近似LRU（Redis采用）：
记录每个键的访问时间戳（精确到毫秒）
淘汰时随机选5个键，淘汰最老的
效果接近真实LRU，但性能好很多
```

**LRU链表示意图**：
```
LRU链表（头部最新，尾部最旧）：

访问key1后：                    内存满，需要淘汰时：
┌────┬────┬────┬────┐          ┌────┬────┬────┬────┐
│key1│key3│key2│key4│          │key1│key3│key2│key4│
└────┴────┴────┴────┘          └────┴────┴────┴────┘
 ↑头                ↑尾                     ↓
                访问时移动到头          从尾部淘汰key4
```

**实现要点**：
1. 定义EvictionPolicy基类，所有替换算法都继承自该类
2. 每个分片维护自己的LRU链表
3. 每个键值对节点包含访问时间戳
4. 访问键时更新访问时间戳，将节点移到LRU链表头部
5. 当内存使用达到上限时，从LRU链表尾部开始淘汰
6. 实现近似LRU算法（类似Redis）：随机抽取5个键，淘汰其中最久未访问的
7. 实现CONFIG SET maxmemory和CONFIG GET maxmemory命令

**类设计**：
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
};
```

**验收标准**：
- 内存使用能被控制在配置的上限以内
- 最久未访问的键优先被淘汰
- 近似LRU算法的准确性达到90%以上
- 淘汰键后内存能正确释放

---

## 章节6：命令增强（第9周）

### 任务6.1：字符串命令增强

**目标**：实现完整的字符串命令

**为什么需要这些命令？**

骨架版本只实现了4个基础命令，完整命令集提供了更丰富的功能：

| 命令类型 | 命令 | 解决的问题 |
|--------|------|-----------|
| **原子计数** | INCR/DECR | 计数器并发安全，比GET+SET+PARSE更快 |
| **字符串操作** | APPEND/STRLEN | 在字符串末尾追加、获取长度 |
| **原子设置** | SETNX/SETEX | 键不存在时设置/带过期时间设置 |

**INCR命令的原子性**：
```cpp
// 非原子方式（多线程不安全）
value = GET key           // 读取
value = value + 1         // 修改
SET key value             // 写回
// 如果两个线程同时执行，可能都读到0，都写成1

// INCR命令（原子操作）
INCR key  // 由内存引擎保证原子性，不会出现上述问题
```

**SETNX的应用场景**：
```
分布式锁：
SETNX lock_key unique_id  # 只有不存在时才能设置成功
# 成功 → 获取锁
# 失败 → 锁已被其他客户端持有
```

**APPEND/STRLEN应用场景**：
```
APPEND：消息追加
127.0.0.1:6379> SET msg ""
OK
127.0.0.1:6379> APPEND msg "hello"
(integer) 5
127.0.0.1:6379> APPEND msg " world"
(integer) 11
127.0.0.1:6379> GET msg
"hello world"

STRLEN：数据长度验证
127.0.0.1:6379> STRLEN msg
(integer) 11
```

**实现要点**：
1. 实现INCR、DECR、INCRBY、DECRBY命令
2. 实现APPEND、STRLEN命令
3. 实现SETNX、SETEX命令
4. 处理边界情况（空字符串、大字符串、整数溢出）

**命令列表**：
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

**验收标准**：
- 所有字符串命令都能正确执行
- 与redis-cli兼容
- 边界情况处理正确（空字符串、大字符串、整数溢出返回错误）

---

## Version2 验收清单

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

## 版本输出

基础版本完成后的项目结构：

```
concurrentcache/
├── src/
│   ├── base/
│   │   ├── log.h/log.cpp           # 完善版日志系统
│   │   ├── config.h/config.cpp     # 完善版配置系统
│   │   ├── signal.h/signal.cpp     # 完善版信号处理
│   │   └── lock.h                  # 各种锁机制
│   ├── memory/
│   │   ├── memory_pool.h/memory_pool.cpp  # 内存池
│   │   ├── thread_cache.h           # ThreadCache
│   │   ├── central_cache.h         # CentralCache
│   │   ├── page_cache.h            # PageCache
│   │   └── span.h                  # Span结构
│   ├── thread/
│   │   ├── thread_pool.h/thread_pool.cpp  # 线程池
│   │   └── task_queue.h            # 任务队列
│   ├── network/
│   │   ├── main_reactor.h/main_reactor.cpp  # MainReactor
│   │   ├── sub_reactor.h/sub_reactor.cpp    # SubReactor
│   │   └── sub_reactor_pool.h      # SubReactor池
│   ├── cache/
│   │   ├── hash_table.h/hash_table.cpp  # 分段锁哈希表
│   │   ├── expire_dict.h            # 过期字典
│   │   ├── eviction_policy.h       # 缓存替换策略基类
│   │   ├── lru_policy.h             # LRU策略
│   │   └── cache_manager.h          # 缓存管理器
│   ├── command/
│   │   └── string_cmd.cpp           # 完整字符串命令
│   └── server/
│       └── main.cpp
├── conf/
│   └── concurrentcache.conf
└── CMakeLists.txt
```

---

## 下一步

基础版本完成后，可以继续开发 **Version3（增强版本）**：

1. **更多数据类型**：哈希、列表、集合、有序集合
2. **更多缓存替换算法**：LFU、FIFO、Random
3. **RDB持久化**：快照持久化
4. **AOF持久化**：追加日志持久化
5. **性能优化**：锁优化、I/O优化、内存优化

详见 `Dev.md` 中的 **Version3：增强版本** 部分。
