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
│ ✅ Logger        ████████████████████████████ 100%         │
│    - 单例模式 + 异步写入 + 文件轮转                          │
│    - v2.0新增: Sink抽象 + 模块分类 + 配置热加载             │
│                                                             │
│ ✅ Config        ████████████████████████████ 100%         │
│    - 单例模式 + key=value解析 + 观察者模式                  │
│    - v2.0完成: 热加载触发机制（定时检查配置文件修改时间）    │
│                                                             │
│ ✅ Signal        ████████████████████████████ 100%         │
│    - SIGINT/SIGTERM 优雅退出                                │
│    - v2.0完成: SIGSEGV堆栈 + SIGPIPE忽略                   │
│                                                             │
│ ✅ Format        ████████████████████████████ 100%         │
│    - 统一格式化工具                                        │
└─────────────────────────────────────────────────────────────┘

【网络通信层】
┌─────────────────────────────────────────────────────────────┐
│ ✅ Socket       ████████████████████████████ 100%             │
│ ✅ Buffer       ████████████████████████████ 100%            │
│ ✅ Channel      ████████████████████████████ 100%            │
│ ✅ EventLoop    ████████████████████████████ 100%            │
│ ✅ Connection   ████████████████████████████ 100%            │
│                                                             │
│ 📋 MainSubReactor ░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
│    - v2.0计划: 多线程Reactor，充分利用多核                    │
└─────────────────────────────────────────────────────────────┘

【缓存核心】
┌─────────────────────────────────────────────────────────────┐
│ ✅ GlobalStorage ████████████████████████████ 100%            │
│    - unordered_map + shared_mutex                           │
│    - v2.0计划: 分段锁哈希表 + 过期 + LRU                   │
│                                                             │
│ 📋 分段锁哈希表 ░░░░░░░░░░░░░░░░░░░░░░░░  0%            │
│ 📋 过期字典     ░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
│ 📋 LRU淘汰      ░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
└─────────────────────────────────────────────────────────────┘

【基础设施】
┌─────────────────────────────────────────────────────────────┐
│ ✅ Format       ████████████████████████████ 100%           │
│    - v2.0新增: 统一格式化工具                               │
│                                                             │
│ 📋 内存池     ░░░░░░░░░░░░░░░░░░░░░░░░░  0%            │
│ 📋 线程池     ░░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
│ 📋 锁机制     ░░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
└─────────────────────────────────────────────────────────────┘

【协议与命令】
┌─────────────────────────────────────────────────────────────┐
│ ✅ RESP协议   ████████████████████████████ 100%              │
│ ✅ CommandFactory ████████████████████████████ 100%          │
│ ✅ 字符串命令 ████████████████████████████ 100%            │
│    - GET/SET/DEL/EXISTS                                   │
│                                                             │
│ 📋 命令增强   ░░░░░░░░░░░░░░░░░░░░░░░░░  0%           │
│    - INCR/DECR/SETNX/SETEX/APPEND/STRLEN                  │
└─────────────────────────────────────────────────────────────┘

图例: ✅ 已完成  🔄 进行中  📋 计划中  百分比 = 估计完成度
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
| 2026-04-25 | Config | ✅ | 热加载触发机制集成（定时检查配置） |

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
- SIGPIPE 忽略（避免写已关闭连接崩溃）

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

### ✅ 网络通信层（Socket/Buffer/Channel/EventLoop/Connection）

**开发时间**：骨架版本

**为什么这样设计**：

```
1. 为什么需要分层？
   - Socket：底层fd操作
   - Buffer：数据暂存，解决粘包
   - Channel：fd + 事件 + 回调
   - EventLoop：事件循环（epoll）
   - Connection：业务层组合

2. 为什么 EventLoop 用 epoll？
   - select/poll O(n) 遍历所有fd
   - epoll O(1) 只处理活跃fd
   - 适合高并发场景

3. 为什么需要非阻塞？
   - 阻塞模式下，accept/recv 会卡住
   - 非阻塞 + epoll 实现事件驱动
```

---

### ✅ RESP 协议

**为什么兼容 Redis 协议**：
- 所有 Redis 客户端直接可用
- 生态系统完善
- 协议简单高效

---

### ✅ CommandFactory（命令工厂）

**为什么用工厂模式**：
- 新增命令不需要改核心代码
- 每个命令独立，易测试
- 动态注册，灵活扩展

---

### ✅ GlobalStorage

**为什么用 shared_mutex**：
- 读多写少场景
- 多个读可以并行
- 写操作独占

---

## 待开发模块

### ✅ Config（配置系统）

**开发时间**：2026-04-24（v2.0 增强）+ 2026-04-25（热加载集成完成）

**v2.0 完成内容**：
- 单例模式（线程安全）
- key=value 配置文件解析
- 观察者模式（ConfigObserver 接口）
- 热加载触发机制（EventLoop 定时检查，每5秒检查一次）
- 默认值设置（log_level, log_file 等）

**核心代码**：
```cpp
// ConfigObserver 接口
class ConfigObserver {
    virtual void onConfigChange(const std::string& key, const std::string& value) = 0;
};

// 热加载触发机制（EventLoop 中）
void EventLoop::check_config_reload() {
    time_t now = time(nullptr);
    if (difftime(now, last_config_check_time_) < config_check_interval_) {
        return;  // 未到检查时间
    }
    last_config_check_time_ = now;
    Config::instance().reload();  // 重新加载配置
    LOG_INFO(event_loop, "配置热加载完成");
}

// 使用示例（main.cpp）
Config::instance().addObserver("log_level", &Logger::instance());
Config::instance().load("./conf/concurrentcache.conf");
```

**文件**：`src/base/config.h`, `src/base/config.cpp`

---

### ✅ Signal（信号处理）

**当前状态**：✅ 已完成（2026-04-25）

**已完成内容**：
- SIGINT/SIGTERM 优雅退出
- SIGSEGV 堆栈捕获（使用 backtrace + dladdr + abi::__cxa_demangle）
- SIGPIPE 忽略（避免写已关闭连接崩溃）

---

### 📋 内存池（三级分层）

**问题**：malloc/free 高并发性能差，有内存碎片

**架构设计**：
```
ThreadCache（TLS无锁）
    ↓ 批量获取/归还
CentralCache（细粒度锁）
    ↓ 按页申请/释放
PageCache（全局锁）
    ↓
系统内存（mmap/brk）
```

**目标**：多线程下性能优于 malloc 3 倍以上

---

### 📋 线程池

**问题**：每任务一线程开销大

**架构设计**：
```
主线程                      工作线程池
  │                             │
  ├─── 提交任务 ──────────► 队列 ◄─── 工作线程
  │                             │
  │◄─── 返回 future ────────────┘
```

---

### 📋 MainSubReactor（多线程网络模型）

**问题**：单 Reactor 只能用到一个 CPU 核心

**架构设计**：
```
MainReactor（主线程/接受连接）
    │
    └── 轮询分发 ──► SubReactor 1（EventLoop + 连接1~500）
                 ──► SubReactor 2（EventLoop + 连接501~1000）
                 ──► SubReactor 3（EventLoop + 连接1001~1500）
                 ──► SubReactor 4（EventLoop + 连接1501~2000）
```

---

### 📋 分段锁哈希表

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

### 📋 过期字典 + LRU

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

### 📋 命令增强

**待实现命令**：
```
原子计数：INCR key / DECR key
条件设置：SETNX key value（键不存在才设置）
过期设置：SETEX key seconds value
字符串操作：APPEND key value / STRLEN key
```

---

## 项目结构

```
concurrentcache/
├── src/
│   ├── base/
│   │   ├── log.h/cpp          ✅ Sink抽象 + 模块 + 热加载
│   │   ├── format.h/cpp       ✅ 统一格式化
│   │   ├── config.h/cpp       ✅ 单例 + 观察者 + 热加载
│   │   └── signal.h/cpp       ✅ SIGSEGV + SIGPIPE
│   │
│   ├── network/
│   │   ├── socket.h
│   │   ├── buffer.h
│   │   ├── channel.h
│   │   ├── event_loop.h/cpp
│   │   └── connection.h/cpp
│   │
│   ├── protocol/
│   │   └── resp.h/cpp
│   │
│   ├── command/
│   │   ├── command.h
│   │   └── string_cmd.h/cpp
│   │
│   └── cache/
│       └── storage.h/cpp
│
├── docs/
│   └── Development/
│       └── Dev_Version1.md     ← 本文档
│
└── conf/
    └── concurrentcache.conf
```

---

## 如何更新本文档

完成一个模块后：

1. **更新进度总览**：把对应模块的 `📋 计划中` 改成 `🔄 进行中`，完成后改成 `✅ 已完成`

2. **记录时间线**：
```markdown
| 日期 | 模块 | 状态 | 关键改动 |
|------|------|------|---------|
| 2026-04-25 | xxx | ✅ | 完成xxx功能 |
```

3. **补充模块详解**（如果有必要说明为什么）：
```markdown
### ✅ 模块名

**开发时间**：2026-04-25

**问题**：解决了什么问题

**为什么这样设计**：
```
1. 为什么需要...
   - 解释
```

**核心代码**：xxx

**文件**：`src/xxx/xxx.h`, `src/xxx/xxx.cpp`
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
| 2026-04-25 | Config | ✅ | 热加载触发机制集成（EventLoop定时检查） |
