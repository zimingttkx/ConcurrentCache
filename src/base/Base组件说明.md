# base 目录组件详解

本文档详细解释 `src/base` 目录下三个基础组件的设计思想、实现原理和使用方式，帮助初学者理解系统开发中的核心概念。

---

## 目录

1. [日志系统 (log.h / log.cpp)](#1-日志系统-logh-logcpp)
2. [配置系统 (config.h / config.cpp)](#2-配置系统-configh-configcpp)
3. [信号处理 (signal.h / signal.cpp)](#3-信号处理-signalh-signalcpp)
4. [锁机制 (lock.h)](#4-锁机制-lockh)

---

## 1. 日志系统 (log.h / log.cpp)

### 1.1 什么是日志系统？

日志系统就像程序的"日记本"——程序运行过程中，把重要的事情记录下来，方便后续查看调试。

**为什么需要专门的日志系统？**
- 控制台输出转瞬即逝，日志文件可以保存
- 多线程同时输出到控制台会乱成一团，日志系统帮你整理好
- 日志太多会占满磁盘，需要自动管理（轮转）
- 日志写入不能拖慢主线程，需要异步处理

### 1.2 核心设计思想

```
┌─────────────────────────────────────────────────────────┐
│                    Logger (单例)                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │                  log() 方法                      │   │
│  │  业务线程调用：格式化日志 → 加入队列 → 返回       │   │
│  └─────────────────────────────────────────────────┘   │
│                          │                              │
│                          ▼ 队列（线程安全）              │
│  ┌─────────────────────────────────────────────────┐   │
│  │           std::queue<std::string>               │   │
│  │  后台写入线程不断从队列中取数据，写入文件         │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**三个关键设计：**

1. **单例模式**：全局只有一个 Logger 实例，通过 `Logger::instance()` 获取
2. **异步写入**：业务线程把日志扔进队列就返回，后台线程负责真正写入文件
3. **批量聚合**：一次 I/O 操作写出多条日志，减少系统调用次数

### 1.3 线程安全基础

**什么是线程安全？**

多个线程同时访问同一块数据，如果不加保护，可能出问题：

```cpp
// 线程 A 和线程 B 同时执行以下代码：
if (queue_.empty()) {     // A 看到空
    // A 刚判断完，还未执行入队
    // B 也看到空
    queue_.push(msg);     // A 入队
    queue_.push(msg);     // B 入队
}
// 结果：只入了 1 条？2 条？不确定！这就是数据竞争
```

**解决方案：互斥锁 (mutex)**

```cpp
std::mutex mutex_;  // 保护队列的锁

void log(...) {
    std::lock_guard<std::mutex> lock(mutex_);  // RAII 风格的加锁
    queue_.push(message);                        // 安全访问
}  // lock_guard 析构时自动解锁
```

`std::lock_guard` 是一个 RAII 包装器，构造时加锁，析构（作用域结束时）时自动解锁，确保锁一定被释放。

### 1.4 条件变量 (condition_variable)

**问题**：后台线程一直在循环检查队列是否为空，浪费 CPU（忙等待）。

**解决**：用条件变量等待，有新数据时再唤醒。

```cpp
std::condition_variable cv_;  // 条件变量

// 后台线程：
std::unique_lock<std::mutex> lock(mutex_);
cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
// 只有当 stop_==true 或 队列非空 时才继续执行

// 业务线程（日志写入时）：
cv_.notify_one();  // 通知后台线程："有数据了，醒来吧！"
```

**为什么用 `std::unique_lock` 而不是 `std::lock_guard`？**

因为 `wait()` 需要在等待期间暂时释放锁，等唤醒时重新加锁。`unique_lock` 支持这种用法。

### 1.5 单例模式的线程安全实现

```cpp
Logger& Logger::instance() {
    static Logger instance;  // C++11 保证静态局部变量线程安全初始化
    return instance;
}
```

这是 **Meyers Singleton**，利用 C++11 的 Magic Static 特性，保证首次调用时线程安全创建实例。

### 1.6 日志轮转 (Log Rotation)

**问题**：日志文件无限增长，撑爆磁盘怎么办？

**解决**：当文件超过一定大小（如 100MB），自动创建新文件。

```
app.log (当前日志)
app.log.1 (最新备份)
app.log.2
app.log.3
...
```

**轮转算法**：

```cpp
void Logger::rotateFile() {
    file_.close();  // 1. 关闭当前文件

    // 2. 移动历史：app.log.2 → app.log.3, app.log.1 → app.log.2
    for (int i = maxFiles_ - 1; i > 0; --i) {
        rename((filepath_ + "." + i).c_str(),
               (filepath_ + "." + (i+1)).c_str());
    }

    // 3. 当前日志重命名为 .1
    rename(filepath_.c_str(), (filepath_ + ".1").c_str());

    // 4. 创建新的空文件
    file_.open(filepath_, std::ios::app);
}
```

### 1.7 时间戳的线程安全处理

**问题**：`ctime()` 使用静态缓冲区，多线程调用会互相覆盖。

**解决**：使用 `std::put_time()` + `std::chrono`。

```cpp
std::string Logger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
```

- `std::chrono::system_clock::now()` 获取高精度时间
- `std::put_time` 是线程安全的格式化函数
- `std::setw(3)` + `setfill('0')` 保证毫秒始终是 3 位数字（如 `.007` 而不是 `.7`）

### 1.8 日志系统使用示例

```cpp
// 设置日志级别（低于此级别的日志被忽略）
Logger::instance().setLevel(LogLevel::INFO);

// 设置输出文件（同时输出到控制台和文件）
Logger::instance().setFile("logs/server.log");

// 设置轮转参数：100MB，保留 5 个历史文件
Logger::instance().setRotation(100 * 1024 * 1024, 5);

// 使用日志宏（printf 风格）
LOG_DEBUG("connection fd=%d", fd);      // 调试信息
LOG_INFO("server started on port %d", 8080);  // 一般信息
LOG_WARN("slow connection");            // 警告
LOG_ERROR("recv failed: %s", strerror(errno));  // 错误

// 程序退出前确保所有日志写入
Logger::instance().flush();
```

---

## 2. 配置系统 (config.h / config.cpp)

### 2.1 什么是配置系统？

配置文件让程序的参数可以在不重新编译的情况下修改。例如服务器端口号、线程数量等。

常见的配置文件格式（`key = value`）：

```
# server.conf
port = 8080
thread_num = 4
log_level = INFO
```

### 2.2 单例模式

配置系统也使用单例模式，确保全局只有一份配置数据：

```cpp
Config& Config::getInstance() {
    static Config instance;
    return instance;
}
```

### 2.3 配置解析流程

```cpp
bool Config::load(const std::string& filename) {
    std::ifstream file(filename);  // 打开文件

    std::string line;
    while (std::getline(file, line)) {  // 逐行读取
        if (line.empty() || line[0] == '#') continue;  // 跳过空行和注释

        size_t pos = line.find('=');  // 找分隔符
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);      // 键
            std::string value = line.substr(pos + 1);   // 值
            trim(key);    // 去除首尾空白
            trim(value);
            config_data_[key] = value;  // 存入 map
        }
    }
}
```

**为什么用 `std::getline` 而不是 `>>` 操作符？**

`>>` 会自动跳过空白，但会把一行中所有空格分隔的内容视为一个 token。`getline` 按行读取，更适合解析 `key = value` 格式。

### 2.4 trim() 的实现原理

```cpp
void Config::trim(std::string& s) {
    if (s.empty()) return;

    // 找到第一个非空白字符的位置，删除之前的所有字符
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));

    // 找到最后一个非空白字符的位置，删除之后的所有字符
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
}
```

`" \t\n\r\f\v"` 包含：空格、水平制表符、换行符、回车符、换页符、垂直制表符。

### 2.5 配置获取接口

```cpp
// 获取字符串配置，默认值为空字符串
std::string Config::getString(const std::string& key,
                              const std::string& default_value) {
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        return it->second;
    }
    return default_value;
}

// 获取整数配置，转换失败返回默认值
int Config::getInt(const std::string& key, int default_value) {
    auto it = config_data_.find(key);
    if (it != config_data_.end()) {
        try {
            return std::stoi(it->second);  // 字符串转整数
        } catch (...) {
            LOG_ERROR("Invalid integer value for key: %s", key.c_str());
        }
    }
    return default_value;
}
```

### 2.6 使用示例

```cpp
// 加载配置文件
Config::getInstance().load("server.conf");

// 读取配置
int port = Config::getInstance().getInt("port", 8080);
int threads = Config::getInstance().getInt("thread_num", 4);
std::string level = Config::getInstance().getString("log_level");
```

---

## 3. 信号处理 (signal.h / signal.cpp)

### 3.1 什么是信号？

信号是操作系统传递给进程的通知，类似于"电话打断"。常见的信号：

| 信号 | 含义 | 默认行为 |
|------|------|----------|
| SIGINT | Ctrl+C | 终止进程 |
| SIGTERM | kill 命令 | 终止进程 |
| SIGSEGV | 段错误 | 终止并生成 core dump |
| SIGHUP | 终端关闭 | 终止进程 |

### 3.2 为什么需要自定义信号处理？

默认的信号处理是直接终止进程。但对于服务器程序，我们希望：
- 收到 Ctrl+C 时，先完成当前请求，再优雅退出
- 收到 SIGTERM 时，清理资源保存状态

### 3.3 信号处理器的实现

```cpp
class SignalHandler {
    std::unordered_map<int, SignalCallback> callbacks_;  // 信号 → 回调函数

    void handle(int signal_num, SignalCallback callback) {
        callbacks_[signal_num] = callback;           // 保存回调
        std::signal(signal_num, signalHandler);       // 注册系统处理器
    }

    static void signalHandler(int signal_num) {
        // 获取单例（因为静态成员函数没有 this 指针）
        auto& instance = getInstance();

        // 查找并执行对应的回调
        if (instance.callbacks_.find(signal_num) != instance.callbacks_.end()) {
            instance.callbacks_[signal_num]();
        }
    }
};
```

**关键点**：
1. `std::signal()` 是 C 标准库函数，用于注册信号处理函数
2. 使用 `std::function` 可以绑定任意可调用对象（函数、lambda、bind 表达式）
3. 静态成员函数 `signalHandler` 没有 `this` 指针，所以通过 `getInstance()` 获取单例

### 3.4 使用示例

```cpp
// 注册 SIGINT 处理：优雅退出
SignalHandler::getInstance().handle(SIGINT, []() {
    LOG_INFO("收到 SIGINT，开始优雅退出...");
    EventLoop::getInstance().quit();
});

// 注册 SIGTERM 处理：快速退出
SignalHandler::getInstance().handle(SIGTERM, []() {
    LOG_INFO("收到 SIGTERM，快速退出...");
    exit(0);
});
```

### 3.5 注意事项

1. **信号处理函数中应尽量少做操作**：信号可以在任何时刻打断程序，处理函数要尽快返回
2. **不要在信号处理函数中调用不安全的函数**：如 `printf`、`malloc` 等
3. **日志系统本身是线程安全的**：可以在信号处理函数中使用 `LOG_*` 宏

---

## 4. 锁机制 (lock.h)

### 4.1 为什么需要锁？

在多线程程序中，多个线程可能同时访问共享资源。如果不加控制，会出现数据竞争：

```cpp
// 线程A和线程B同时执行：
// 初始时 value = 0

// 线程A读取 value (得到0)
// 线程B读取 value (得到0)
// 线程A计算 value + 1 (得到1)
// 线程B计算 value + 1 (得到1)
// 线程A写入 value = 1
// 线程B写入 value = 1

// 结果：value = 1（但我们期望是2！）
```

**解决方案：用锁把"读取-修改-写入"变成原子的。**

### 4.2 原子操作

原子操作在执行过程中不会被其他线程干扰，比锁更轻量：

```cpp
// 原子整数 - 比 mutex 更轻量
AtomicInteger counter(0);
counter++;                      // 线程安全地加1
counter.fetch_add(10);         // 线程安全地加10
int val = counter.load();      // 读取当前值
```

**为什么原子操作更快？**
- 锁需要：加锁 → 操作 → 解锁，涉及内核态切换
- 原子操作在硬件层面保证原子性，用户态即可完成

### 4.3 各种锁的选择

| 锁类型 | 适用场景 | 特点 |
|--------|----------|------|
| **Mutex** | 一般同步 | 支持超时获取 |
| **SpinLock** | 临界区极短（<100条指令） | 自旋等待，不挂起线程 |
| **RecursiveMutex** | 同一线程多次获取 | 递归调用时不会死锁 |
| **RWLock** | 读多写少 | 读可并发，写必须独占 |
| **RWLock2** | 写优先场景 | 避免写饥饿 |

**SpinLock vs Mutex 怎么选？**

```
┌─────────────────────────────────────────────────────────┐
│  临界区执行时间    │           推荐                        │
├─────────────────────────────────────────────────────────┤
│  < 100 条指令     │  SpinLock（自旋，不挂起线程）         │
│  > 1000 条指令    │  Mutex（挂起，不消耗CPU）            │
└─────────────────────────────────────────────────────────┘
```

### 4.4 读写锁详解

读写锁的核心思想：
- **读操作不修改数据**：多个线程同时读是安全的
- **写操作会修改数据**：必须独占

```cpp
RWLock rwlock;
std::string cache;

// 多个线程可以同时读取
void read_data() {
    ReadLockGuard<RWLock> guard(rwlock);  // 读锁
    std::string data = cache;  // 安全读取
}

// 只有一个线程能写入
void write_data(const std::string& new_data) {
    WriteLockGuard<RWLock> guard(rwlock);  // 写锁
    cache = new_data;  // 安全写入
}
```

### 4.5 分片锁 - 减少锁竞争

**问题**：全局只有一把锁，所有线程都要竞争。

**解决**：把锁分散成多个分片，访问不同数据的线程用不同的锁。

```
┌─────────────────────────────────────────────────────────┐
│  全局锁（不好）                                          │
│  线程A访问数据1，线程B访问数据2 → 也要竞争同一把锁      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  分片锁（好）                                           │
│  线程A访问key1 → 分片0 → 锁0                           │
│  线程B访问key2 → 分片1 → 锁1                           │
│  两个线程完全并行，互不干扰！                           │
└─────────────────────────────────────────────────────────┘
```

```cpp
ShardedLock<SpinLock> locks(16);  // 16个分片

void access(const std::string& key) {
    auto& lock = locks.get_shard(key);  // 根据key选择分片
    LockGuard<SpinLock> guard(lock);
    // 访问该分片的数据
}
```

### 4.6 RAII 锁守卫 - 防止死锁

**问题**：忘记 `unlock()` 导致死锁。

```cpp
// 危险写法
mutex.lock();
if (something) {
    return;  // 忘记 unlock！死锁！
}
mutex.unlock();
```

**解决：LockGuard 自动管理锁的获取和释放。**

```cpp
// 安全写法
{
    LockGuard<std::mutex> guard(mutex);  // 构造函数加锁
    if (something) {
        return;  // guard析构，自动解锁
    }
}  // guard析构，自动解锁
```

无论函数如何退出（正常 return、异常、goto），LockGuard 都会确保锁被释放。

### 4.7 同步原语

**Semaphore（信号量）**：控制同时访问的线程数量。

```cpp
Semaphore sem(3);  // 最多3个线程同时访问

// 线程A
sem.wait();   // 计数从3变成2
// 访问共享资源
sem.post();   // 计数从2变成3
```

**CountDownLatch（倒计时门栓）**：等待N个操作完成。

```cpp
CountDownLatch latch(3);  // 3个线程

// 主线程等待
latch.wait();  // 阻塞，直到计数变为0

// 线程函数
void worker() {
    // 做工作...
    latch.count_down();  // 计数-1
}
```

**CyclicBarrier（循环屏障）**：N个线程互相等待。

```cpp
CyclicBarrier barrier(3);  // 3个线程

void worker(int id) {
    // 第一阶段：各自准备
    prepare();
    barrier.wait();  // 3个线程都在这里等待
    // 所有线程都到达后，一起继续执行
    execute();
}
```

### 4.8 伪共享与缓存行对齐

**问题**：多个线程访问同一个缓存行中的不同变量，导致缓存行频繁失效。

```
CPU缓存行（64字节）：
[变量A: 8字节][变量B: 8字节][变量C: 8字节]...[填充]

线程1修改变量A → 线程2修改变量B → 整个缓存行失效，需要重新加载
```

**解决：使用 `alignas(64)` 对齐到缓存行。**

```cpp
struct alignas(64) Counter {
    std::atomic<uint64_t> counter{0};
};
```

### 4.9 常见问题

**Q: 什么是死锁？如何避免？**
A：两个线程互相等待对方释放锁。避免方法：1) 用 RAII 锁守卫 2) 总是按相同顺序获取锁

**Q: 什么是惊群效应？**
A：锁释放时，所有等待线程都被唤醒，但只有一个能获取锁。解决：使用退避策略，让线程在不同时间醒来

**Q: `= delete` 是什么意思？**
A：禁用该函数。比如 `AtomicInteger(const AtomicInteger&) = delete` 禁用拷贝构造函数

### 4.10 使用示例

```cpp
// 1. 基本互斥锁
Mutex mutex;
{
    MutexGuard guard(mutex);  // 自动加锁
    // 临界区
}

// 2. 读写锁
RWLock rwlock;
{
    RWLockReadGuard guard(rwlock);   // 读锁
    // 读取共享数据
}
{
    RWLockWriteGuard guard(rwlock);  // 写锁
    // 写入共享数据
}

// 3. 分片锁（高并发场景）
ShardedLock<SpinLock> locks(16);
auto& lock = locks.get_shard(key);
SpinLockGuard guard(lock);

// 4. 原子操作
AtomicInteger counter(0);
counter++;
int val = counter.load();
```

---

## 5. 常见问题解答

### Q: 为什么要用单例模式？

A：确保全局只有一个实例，避免多处配置冲突。对于 Logger、Config、SignalHandler 这类全局管理器，单例是最简单合理的选择。

### Q: 异步日志会不会丢日志？

A：不会。日志进入队列后，后台线程负责写出。如果程序异常退出，队列中未写出的日志会丢失，但这是可接受的设计权衡（性能 vs 可靠性）。

### Q: 条件变量为什么要用 `unique_lock` 而不是 `lock_guard`？

A：因为 `wait()` 会释放锁让线程睡眠，醒来时重新加锁。`unique_lock` 支持这种锁定-解锁-重新锁定的操作，而 `lock_guard` 一旦构造就不可解锁。

### Q: `std::function` 是什么？

A：是一个通用的可调用对象包装器，可以存储、复制和调用任何可调用目标（函数指针、lambda、bind 表达式、重载了 `operator()` 的类等）。

```cpp
std::function<void()> callback = []() { LOG_INFO("hello"); };
callback();  // 调用 lambda
```

---

## 6. 代码逻辑流程图

### 日志写入流程

```
业务线程调用 log()
        │
        ▼
检查日志级别（DEBUG/INFO/WARN/ERROR）
        │
        ▼
格式化日志消息（vsnprintf）
        │
        ▼
加锁，入队到 queue_
        │
        ▼
通知 cv_（条件变量）
        │
        ▼
立即返回（不阻塞）

──────────────────────────────────

后台 writerThread 循环
        │
        ▼
wait() 等待条件变量
        │
        ▼
被 notify 唤醒，加锁
        │
        ▼
批量取出队列中所有日志
        │
        ▼
解锁，批量写入文件
        │
        ▼
检查是否需要轮转
        │
        ▼
继续等待
```

### 配置加载流程

```
load("server.conf")
        │
        ▼
打开文件
        │
        ▼
while (getline 逐行读取)
        │
        ├── 空行或 # 开头 → 跳过
        │
        ├── 找 '=' 分隔符
        │        │
        │        ▼
        │   分割 key 和 value
        │        │
        │        ▼
        │   trim() 去除首尾空格
        │        │
        │        ▼
        │   config_data_[key] = value
        │
        └── 文件结束 → 退出循环
                │
                ▼
返回成功
```

---

## 7. 进一步学习建议

1. **C++11 并发编程**：《C++ Concurrency in Action》 chapter 4（条件变量和互斥锁）
2. **设计模式**：《设计模式》单例模式、RAII 惯用法
3. **Linux 系统编程**：《Linux 高性能服务器编程》信号处理章节
4. **工程实践**：阅读 Boost.Log、spdlog 等成熟日志库的源码
5. **并发锁机制**：阅读 Linux 内核中的 futex 机制、CPU 缓存一致性协议（MESI）
6. **性能优化**：了解伪共享、缓存行对齐、无锁数据结构（如原子操作）
