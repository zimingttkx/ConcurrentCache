# ConcurrentCache

一个从零开始实现的 C++ 高性能内存对象缓存系统，兼容 Redis RESP 协议。

## 项目目标

- 深入理解计算机网络底层原理和高性能网络编程
- 掌握 C++ Linux 环境下的网络编程技术
- 学习 Reactor 事件驱动模型和 epoll 多路复用
- 理解缓存系统的核心设计思想

## 当前版本状态

**V1.0** - 基础框架版本（已发布）

## 技术栈

- **编程语言**：C++17
- **操作系统**：Linux
- **网络模型**：单 Reactor + epoll（LT 水平触发）
- **协议支持**：Redis RESP 协议

## 已实现功能

### 核心组件

| 模块 | 功能 |
|------|------|
| EventLoop | 基于 epoll 的单 Reactor 事件循环 |
| Connection | TCP 连接管理，缓冲区读写 |
| Socket | Socket 封装，bind/listen/accept |
| Buffer | 输入输出缓冲区，解决 TCP 粘包问题 |

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

### 基础组件

| 模块 | 功能 |
|------|------|
| Logger | 日志系统，支持多级别输出 |
| Config | 配置管理模块 |
| Signal | 信号处理（SIGSEGV 堆栈捕获、SIGPIPE 忽略） |
| Format | 字符串格式化工具 |

## 架构概览

```
┌─────────────────────────────────────────┐
│              redis-cli                   │
└─────────────────┬───────────────────────┘
                  │ TCP (RESP)
┌─────────────────▼───────────────────────┐
│              EventLoop                  │
│         (单 Reactor + epoll)           │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────▼───────────────────────┐
│           Connection Manager            │
│    Socket → Buffer → RespParser        │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────▼───────────────────────┐
│           Command Dispatcher            │
│      CommandFactory → Command           │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────▼───────────────────────┐
│           GlobalStorage                 │
│        (全局哈希表存储)                  │
└─────────────────────────────────────────┘
```

## 快速开始

### 环境要求

- Linux 系统
- C++17 编译器
- CMake 3.10+
- spdlog

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
./concurrent_cache_server
```

### 测试

```bash
# 使用 redis-cli 连接
redis-cli -p 6379

# 测试命令
127.0.0.1:6379> SET name test
OK
127.0.0.1:6379> GET name
"test"
127.0.0.1:6379> EXISTS name
(integer) 1
127.0.0.1:6379> DEL name
(integer) 1
```

## 项目结构

```
src/
├── base/           # 基础组件
│   ├── log.cpp/h  # 日志系统
│   ├── config.cpp/h # 配置管理
│   ├── signal.cpp/h # 信号处理
│   └── format.cpp/h # 格式化工具
├── network/        # 网络层
│   ├── socket.cpp/h # Socket 封装
│   ├── event_loop.cpp/h # 事件循环
│   ├── channel.cpp/h # 事件通道
│   ├── connection.cpp/h # 连接管理
│   └── buffer.cpp/h # 缓冲区
├── protocol/       # 协议层
│   └── resp.cpp/h # RESP 协议解析
├── command/        # 命令层
│   ├── command.h  # 命令基类
│   ├── command_factory.cpp/h # 命令工厂
│   └── string_cmd.h # 字符串命令实现
└── cache/          # 缓存层
    └── storage.cpp/h # 全局存储
```

## 开发计划

| 版本 | 目标 |
|------|------|
| V1.0 | 基础框架，单 Reactor，GET/SET/DEL/EXISTS |
| V2.0 | 日志系统重构，信号处理增强，内存池，线程池 |
| V3.0 | 主从 Reactor 模型，分段锁哈希表，LRU 淘汰策略 |
| V4.0 | 多种数据类型，RDB/AOF 持久化 |
| V5.0 | 集群模式，哈希槽分片，主从复制 |

## 参考资料

- [Redis 设计与实现](https://github.com/huangz1990/redisbook)
- [muduo 网络库](https://github.com/chenshuo/muduo)
- [Linux 高性能服务器编程](https://book.douban.com/subject/24772279/)

## 许可证

MIT License - 详见 [LICENSE](LICENSE)
