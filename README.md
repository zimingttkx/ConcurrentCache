# ConcurrentCache

一个从零开始实现的 C++ 高性能内存对象缓存系统，兼容 Redis RESP 协议。

## 项目目标

- 深入理解计算机网络底层原理和高性能网络编程
- 掌握 C++ Linux 环境下的网络编程技术
- 学习 Reactor 事件驱动模型和 epoll 多路复用
- 理解缓存系统的核心设计思想

## 当前版本状态

**V2.0** - 基础版本（已发布）
- MainSubReactor 多线程网络模型
- 三级内存池
- 完整锁机制
- 线程池
- 优雅退出

## 技术栈

- **编程语言**：C++20
- **操作系统**：Linux
- **网络模型**：MainReactor + SubReactorPool（多线程 Reactor）
- **协议支持**：Redis RESP 协议

## 已实现功能

### 核心组件

| 模块 | 功能 |
|------|------|
| MainReactor | 单线程处理 accept |
| SubReactorPool | 多线程处理 I/O，轮询负载均衡 |
| EventLoop | 基于 epoll 的事件循环 |
| Connection | TCP 连接管理，缓冲区读写 |
| Socket | Socket 封装，bind/listen/accept |
| Buffer | 输入输出缓冲区，解决 TCP 粘包问题 |

### 线程池

| 模块 | 功能 |
|------|------|
| ThreadPool | 通用线程池，支持任务提交和优雅退出 |

### 协议层

| 模块 | 功能 |
|------|------|
| RespParser | RESP 协议解析器 |
| RespEncoder | RESP 协议编码器 |
| RespValue | RESP 数据结构封装 |

### 命令支持

| 命令 | 功能 |
|------|------|
| GET | 获取键值 |
| SET | 设置键值 |
| DEL | 删除键 |
| EXISTS | 检查键是否存在 |
| PING | 心跳检测 |

### 基础组件

| 模块 | 功能 |
|------|------|
| Logger | 日志系统，支持多级别输出、Sink 抽象、热加载 |
| Config | 配置管理模块，支持热加载 |
| Signal | 信号处理（SIGSEGV 堆栈捕获、SIGPIPE 忽略） |
| Format | 字符串格式化工具 |
| Lock | 完整锁机制（Mutex/SpinLock/RWLock/Semaphore 等） |

### 并发工具

| 模块 | 功能 |
|------|------|
| AtomicInteger | 原子整数操作 |
| AtomicPointer | 原子指针操作 |
| Mutex | 互斥锁（支持超时） |
| SpinLock | 自旋锁 |
| RWLock | 读写锁 |
| ShardedLock | 分片锁 |
| Semaphore | 信号量 |
| CountDownLatch | 倒计时门栓 |
| CyclicBarrier | 循环屏障 |

### 内存管理

| 模块 | 功能 |
|------|------|
| ThreadCache | 线程本地缓存，无锁分配 |
| CentralCache | 中心缓存，细粒度锁 |
| PageCache | 页缓存，直接和系统交互 |

## 架构概览

```
┌─────────────────────────────────────────┐
│              redis-cli                   │
└─────────────────┬───────────────────────┘
                  │ TCP (RESP)
┌─────────────────▼───────────────────────┐
│           MainReactor                    │
│      (单线程处理 accept)                │
└─────────────────┬───────────────────────┘
                  │ 新连接分发 (轮询)
┌─────────────────▼───────────────────────┐
│         SubReactorPool                  │
│    ┌──────────┐ ┌──────────┐          │
│    │SubReactor│ │SubReactor│ ...      │
│    │  线程0   │ │  线程1   │          │
│    └─────┬────┘ └─────┬────┘          │
│          │            │                 │
│    ┌─────▼────┐ ┌─────▼────┐          │
│    │EventLoop │ │EventLoop │          │
│    └──────────┘ └──────────┘          │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│           ThreadPool                     │
│    (4 工作线程，异步任务处理)            │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│        GlobalStorage                      │
│     (全局哈希表存储，线程安全)           │
└─────────────────────────────────────────┘
```

## 快速开始

### 环境要求

- Linux 系统
- C++20 编译器
- CMake 3.28+
- spdlog

### 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行

```bash
./concurrentcache-server
```

### 测试

```bash
# 使用 redis-cli 连接
redis-cli -p 6379

# 测试命令
127.0.0.1:6379> PING
PONG
127.0.0.1:6379> SET name test
OK
127.0.0.1:6379> GET name
"test"
127.0.0.1:6379> EXISTS name
(integer) 1
127.0.0.1:6379> DEL name
(integer) 1
```

### 优雅退出

服务器支持通过信号优雅退出：

```bash
# Ctrl+C 或 kill -SIGINT <pid>
# 服务器将：
# 1. 停止接受新连接
# 2. 等待现有连接处理完成
# 3. 安全关闭所有线程
# 4. 释放资源并退出
```

## 项目结构

```
src/
├── base/           # 基础组件
│   ├── log.cpp/h  # 日志系统
│   ├── config.cpp/h # 配置管理
│   ├── signal.cpp/h # 信号处理
│   ├── format.cpp/h # 格式化工具
│   ├── lock.cpp/h  # 锁机制
│   └── thread_pool.cpp/h # 线程池
├── network/        # 网络层
│   ├── socket.cpp/h # Socket 封装
│   ├── event_loop.cpp/h # 事件循环
│   ├── channel.cpp/h # 事件通道
│   ├── connection.cpp/h # 连接管理
│   ├── main_reactor.cpp/h # MainReactor
│   ├── sub_reactor.cpp/h # SubReactor
│   └── sub_reactor_pool.cpp/h # SubReactorPool
├── memorypool/     # 内存池
│   ├── size_class.cpp/h
│   ├── free_list.cpp/h
│   ├── span.cpp/h
│   ├── page_cache.cpp/h
│   ├── central_cache.cpp/h
│   ├── thread_cache.cpp/h
│   └── memory_pool.h
├── protocol/       # 协议层
│   └── resp.cpp/h # RESP 协议解析
├── command/        # 命令层
│   ├── command.h  # 命令基类
│   ├── command_factory.cpp/h # 命令工厂
│   └── string_cmd.h # 字符串命令实现
└── cache/          # 缓存层
    └── storage.cpp/h # 全局存储

test/               # 测试套件
├── trace/         # 测试框架
├── lock_test/     # 锁测试
├── atomic_test/   # 原子操作测试
└── sync_primitives_test/ # 同步原语测试

conf/               # 配置文件
└── concurrentcache.conf

docs/
└── developing/     # 开发文档
    ├── Dev.md
    ├── Architecture.md
    └── Roadmap.md
```

## 开发计划

| 版本 | 目标 |
|------|------|
| V1.0 | 基础框架，单 Reactor，GET/SET/DEL/EXISTS |
| V2.0 | 线程池，MainSubReactor，内存池，锁机制，优雅退出 |
| V3.0 | 主从 Reactor 模型，分段锁哈希表，LRU 淘汰策略 |
| V4.0 | 多种数据类型，RDB/AOF 持久化 |
| V5.0 | 集群模式，哈希槽分片，主从复制 |

## 测试

项目包含完整的测试套件：

```bash
# 锁正确性测试
./build/test/test_lock_correctness

# 死锁检测测试
./build/test/test_lock_deadlock

# 数据竞争测试
./build/test/test_lock_race

# 压力测试
./build/test/test_lock_stress

# 边界测试
./build/test/test_lock_boundary

# 原子操作测试
./build/test/test_atomic_correctness

# 同步原语测试
./build/test/test_sync_primitives
```

## 参考资料

- [Redis 设计与实现](https://github.com/huangz1990/redisbook)
- [muduo 网络库](https://github.com/chenshuo/muduo)
- [Linux 高性能服务器编程](https://book.douban.com/subject/24772279/)

## 许可证

MIT License - 详见 [LICENSE](LICENSE)
