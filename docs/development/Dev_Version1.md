# ConcurrentCache 骨架版本（Version1）

## 版本概述

**目标**：构建项目最小可运行版本，让整个系统能够跑起来。

骨架版本是整个项目的根基，必须稳扎稳打，确保每个模块都能正常工作。本版本实现一个简化但完整的缓存服务器，支持基本的 GET/SET/DEL 命令。

**预计开发周期**：2-3周

**核心功能**：
- 单Reactor网络模型
- RESP协议解析
- 简单命令分发
- 基础内存存储
- GET/SET/DEL命令

---

## 组件作用说明

### 为什么要开发骨架版本？

骨架版本是整个项目的根基，就像建房子需要地基一样。虽然功能简单，但它是后续所有功能的基础：

| 组件 | 作用 | 为什么需要 |
|-----|------|----------|
| **Logger** | 记录程序运行日志 | 排查问题、监控状态、调试代码 |
| **Config** | 读取配置文件 | 灵活配置服务器参数，无需硬编码 |
| **Signal** | 处理系统信号 | 优雅退出、优雅重启 |
| **Socket** | 网络通信基础 | 客户端连接、数据传输 |
| **Buffer** | 管理收发数据 | 解决TCP粘包、数据缓存 |
| **Channel** | 管理文件描述符事件 | 连接读写事件通知 |
| **EventLoop** | 事件驱动核心 | 高效处理大量并发连接 |
| **Connection** | 管理客户端连接 | 封装连接状态、数据收发 |
| **RESP协议** | 协议解析/序列化 | 与Redis客户端兼容 |
| **Command** | 命令分发路由 | 扩展性强、新命令易添加 |
| **GlobalStorage** | 内存数据存储 | 核心缓存功能 |

---

## 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         骨架版本架构                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐         ┌─────────────┐         ┌─────────────┐  │
│  │   Logger    │         │   Config    │         │   Signal    │  │
│  │  （简化版）  │         │  （基础版）  │         │  （基础版）  │  │
│  └─────────────┘         └─────────────┘         └─────────────┘  │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     单Reactor网络模型                         │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │   │
│  │  │  Socket  │→│  Buffer  │→│  Channel │→│EventLoop │  │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     核心功能                                  │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │   │
│  │  │  RESP    │→│  命令    │→│  存储    │→│ Response │   │   │
│  │  │  解析器   │  │  分发    │  │ (哈希表) │  │  封装     │   │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     命令实现                                  │   │
│  │              GET / SET / DEL / EXISTS                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 项目目录结构

```
concurrentcache/
├── src/
│   ├── base/
│   │   ├── log.h           # 简化版日志系统
│   │   ├── config.h        # 简化版配置系统
│   │   └── signal.h        # 简化版信号处理
│   ├── network/
│   │   ├── socket.h        # Socket封装
│   │   ├── buffer.h        # 输入输出缓冲区
│   │   ├── channel.h       # Channel封装
│   │   ├── event_loop.h    # EventLoop（单Reactor）
│   │   ├── event_loop.cpp  # EventLoop实现
│   │   └── connection.h    # 连接管理
│   │   └── connection.cpp   # Connection实现
│   ├── protocol/
│   │   └── resp.h          # RESP协议解析器
│   ├── command/
│   │   ├── command.h       # 命令基类和工厂
│   │   └── string_cmd.h    # 字符串命令实现
│   ├── cache/
│   │   └── hash_table.h    # 简单哈希表（可选，后续版本）
│   └── server/
│       └── main.cpp         # 程序入口
├── tests/
│   └── unit/               # 单元测试
├── conf/
│   └── concurrentcache.conf      # 配置文件
├── CMakeLists.txt
└── README.md
```

---

## 章节1：项目初始化（第1天）

### 任务1.1：创建项目目录结构

**目标**：创建骨架版本项目目录结构

**实现要点**：
1. 创建 src/base/、src/network/、src/protocol/、src/command/、src/cache/、src/server/ 目录
2. 创建 tests/unit/ 目录
3. 创建 conf/ 目录

---

### 任务1.2：配置CMake构建系统

**目标**：编写CMakeLists.txt，配置C++17标准和编译选项

**实现要点**：
1. 配置C++17标准
2. 开启编译警告：-Wall -Wextra -Werror -O2
3. 引入spdlog依赖（find_package）
4. 配置可执行文件输出路径
5. 链接pthread

---

### 任务1.3：编写配置文件和.gitignore

**目标**：创建配置文件和git忽略规则

**实现要点**：
1. 编写 concurrentcache.conf，包含 port、thread_num 配置项
2. 编写 .gitignore，忽略 build/、*.o、*.a、可执行文件等

**验收标准**：
- `mkdir build && cd build && cmake .. && make` 能成功编译
- 生成的 `concurrentcache-server` 可执行文件能运行

---

## 章节2：基础工具模块（第2-4天）

### 任务2.1：简化版日志系统

**目标**：基于spdlog封装一个简单的日志系统

**为什么需要这个组件？**

日志系统是排查问题的眼睛，没有日志就像盲人摸象。在缓存服务器中，日志可以告诉我们：

1. **程序是否正常启动**：服务器监听了哪个端口
2. **客户端做了什么操作**：哪个客户端执行了什么命令
3. **出了问题在哪里**：错误日志帮助快速定位问题
4. **运行状态怎么样**：关键指标的日志输出

**使用场景示例**：
```
[2024-01-15 10:30:15.123] [INFO] Server started on port 6379
[2024-01-15 10:30:20.456] [INFO] Client connected: fd=5, ip=127.0.0.1
[2024-01-15 10:30:25.789] [DEBUG] Command received: SET key1 value1
[2024-01-15 10:30:25.790] [ERROR] Connection error: fd=5, error=104
```

**实现要点**：
1. 使用spdlog实现日志输出
2. 支持4种日志级别：DEBUG、INFO、WARN、ERROR
3. 支持控制台输出
4. 提供全局日志宏：LOG_DEBUG、LOG_INFO、LOG_WARN、LOG_ERROR
5. 初始化函数 init_log()

**验收标准**：
- LOG_INFO("Server started on port {}", port) 能输出日志

---

### 任务2.2：简化版配置系统

**目标**：实现一个简单的配置读取功能

**为什么需要这个组件？**

配置系统让服务器的行为可以通过配置文件调整，不需要修改代码就能改变服务器运行方式：

1. **灵活性**：端口号、最大连接数等参数可配置
2. **安全性**：敏感配置可以通过配置文件提供，不硬编码
3. **运维友好**：不同环境（开发、测试、生产）使用不同配置
4. **部署简单**：同一套二进制，配合不同配置文件适用不同场景

**使用场景示例**：
```ini
# concurrentcache.conf
port = 6379           # 服务器监听端口
bind = 0.0.0.0        # 绑定地址
max_clients = 10000   # 最大客户端数
thread_num = 4        # 线程数
```

**为什么需要单例模式？**
- 配置在程序生命周期内只需要一份
- 全局访问点方便各模块获取配置
- 避免重复读取配置文件

**实现要点**：
1. 使用std::ifstream读取配置文件
2. 解析 key = value 格式（支持 # 注释和空行跳过）
3. 提供 get_int()、get_string() 方法
4. 提供单例模式 instance()
5. 支持默认值
6. 配置项：port、thread_num

**验收标准**：
- 能读取 port = 6379 并正确解析为整数
- 未配置的项返回默认值

---

### 任务2.3：简化版信号处理

**目标**：实现基本的信号处理，支持优雅退出

**为什么需要这个组件？**

信号处理让服务器能够响应操作系统发来的通知，优雅地处理退出请求：

1. **SIGINT（Ctrl+C）**：用户中断程序，服务器应该优雅退出
2. **SIGTERM**：系统管理员关闭服务，服务器应该优雅退出
3. **优雅退出**：处理完当前请求再退出，不丢失数据
4. **资源清理**：退出时关闭文件描述符、释放内存

**使用场景示例**：
```bash
# 启动服务器
./concurrentcache-server &

# Ctrl+C 发送SIGINT
^C
# 服务器输出: Received SIGINT, shutting down gracefully...
# 服务器等待当前请求处理完毕
# 服务器输出: Server stopped

# 或者使用kill命令
kill -TERM $(pidof concurrentcache-server)
```

**为什么使用atomic_bool？**
- 信号处理函数在独立上下文执行
- atomic_bool保证多线程/信号处理函数间的安全通信
- 相比普通bool，避免了数据竞争

**实现要点**：
1. 使用std::atomic<bool>作为退出标志
2. 处理SIGINT、SIGTERM信号
3. 信号处理函数设置退出标志为true
4. 提供 init_signal() 初始化函数
5. 提供 is_running() 查询运行状态

**验收标准**：
- Ctrl+C 能设置退出标志
- 程序能检测到退出标志并优雅退出

---

## 章节3：网络通信层（第5-10天）

### 任务3.1：Socket封装

**目标**：封装Linux Socket API，实现基础TCP通信

**为什么需要这个组件？**

Socket是网络通信的基础，所有网络连接都依赖它。就像盖房子需要砖块一样，网络编程需要Socket：

1. **服务器监听**：bind()绑定端口，listen()开始监听
2. **接受连接**：accept()接受客户端连接请求
3. **数据传输**：send()/recv()发送和接收数据
4. **资源管理**：通过RAII自动管理socket生命周期

**TCP通信基本流程**：
```
服务器端：                           客户端：
socket()                          socket()
    ↓                                  ↓
bind(port)                        connect(server)
    ↓                                  ↓
listen()                          send(data)
    ↓                                  ↓
accept()←──────────────────connect()
    ↓                                  ↓
recv()/send()←────────────────recv()/send()
    ↓                                  ↓
close()                          close()
```

**为什么需要非阻塞模式？**
- 阻塞模式下accept()会一直等待连接
- 阻塞模式下recv()/send()会一直等待数据
- 非阻塞模式配合epoll，实现高效的事件驱动

**实现要点**：
1. 封装 socket()、bind()、listen()、accept()
2. 封装 connect()（用于测试）
3. 支持设置非阻塞模式 set_nonblocking()
4. 支持地址复用 SO_REUSEADDR
5. 提供 send()、recv()、close() 方法
6. 支持移动语义（禁用拷贝）

**类设计**：
```cpp
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd);
    ~Socket();

    // 移动语义
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // 禁用拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // 创建监听socket
    bool bind_and_listen(int port, bool reuse = true);

    // 接受连接
    int accept();

    // 连接到服务器
    bool connect(const std::string& ip, int port);

    // 发送/接收数据
    ssize_t send(const void* buf, size_t len);
    ssize_t recv(void* buf, size_t len);

    // 设置非阻塞
    void set_nonblocking(bool enable);

    // 获取fd
    int fd() const;
    void close();
};
```

**验收标准**：
- 能绑定端口并监听
- 能接受客户端连接
- 非阻塞模式工作正常

---

### 任务3.2：输入输出缓冲区

**目标**：实现可动态扩展的读写缓冲区

**为什么需要这个组件？**

Buffer解决了网络编程中的两个核心问题：

1. **TCP粘包问题**：TCP是字节流协议，不保留消息边界。一次recv()可能收到半个命令或多个命令拼接在一起
2. **读写速度不匹配**：发送方和接收方速度可能不同，需要缓冲区暂存

**粘包问题示意图**：
```
发送的数据（多条命令）：
"GET key1\r\nSET key2 value2\r\nGET key3\r\n"

recv()可能的结果：
- 一次接收完整："GET key1\r\nSET key2 value2\r\nGET key3\r\n"
- 分多次接收："GET key1\r\nSET key2 value2\r\n" + "GET key3\r\n"
- 拆分更细："GET key1\r" + "\nSET key2 value2\r\n" + "GET key3\r\n"
```

**读写指针分离设计**：
```
Buffer内部结构：
┌─────────────────────────────────────────────────────┐
│ 已读数据 │            可读数据             │ 可写空间 │
└─────────┴──────────────────────────────────┴─────────┘
          ↑reader_index_                   ↑writer_index_

retrieve(len)：移动reader_index_，释放已读空间
append()：移动writer_index_，扩展缓冲区
```

**实现要点**：
1. 使用vector<char>存储数据
2. 支持读写指针分离（reader_index_、writer_index_）
3. 支持自动扩容
4. 提供 append() 追加数据
5. 提供 peek() 读取数据（不删除）
6. 提供 retrieve() 删除已读数据
7. 提供 readable_bytes()、writable_bytes() 查询大小

**验收标准**：
- 缓冲区读写操作正确
- 缓冲区能自动扩容

**类设计**：
```cpp
class Buffer {
public:
    // 可读字节数
    size_t readable_bytes() const;

    // 可写字节数
    size_t writable_bytes() const;

    // 追加数据
    void append(const char* data, size_t len);

    // 读取数据（不删除）
    const char* peek() const;

    // 删除已读数据
    void retrieve(size_t len);
    void retrieve_all();

    // 获取字符串
    std::string to_string() const;
};
```

**验收标准**：
- 缓冲区读写操作正确
- 缓冲区能自动扩容

---

### 任务3.3：Channel类

**目标**：封装文件描述符和其感兴趣的事件

**为什么需要这个组件？**

Channel将文件描述符（socket）和其相关的事件处理解耦，让EventLoop能够统一管理所有连接：

1. **事件封装**：将fd和其感兴趣的事件（读/写）打包在一起
2. **回调管理**：保存读事件和写事件的处理函数
3. **统一接口**：不管是socket还是pipe，都用相同的接口管理

**Channel在Epoll中的作用**：
```
Epoll注册的是Channel对象，不是裸fd：
- fd: 5 (socket的文件描述符)
- events: EPOLLIN | EPOLLOUT (感兴趣的事件)
- read_callback: handle_read函数
- write_callback: handle_write函数

Epoll事件触发时：
- 调用channel->handle_event()
- 根据触发的事件类型调用相应回调
```

**为什么需要update()函数？**
- Channel创建后需要注册到epoll
- 事件类型改变时需要更新epoll
- remove_channel()从epoll中移除

**实现要点**：
1. 保存fd和感兴趣的事件（events_）
2. 保存所属的EventLoop指针
3. 设置回调函数（read_callback_、write_callback_）
4. 设置感兴趣的事件：enable_reading()、enable_writing()、disable_all()
5. handle_event() 处理事件时调用相应回调
6. update() 更新到epoll

**验收标准**：
- Channel能正确保存fd和事件
- 回调函数能正确调用

**类设计**：
```cpp
class Channel {
public:
    using ReadCallback = std::function<void()>;
    using WriteCallback = std::function<void()>;

    Channel(int fd, EventLoop* loop);
    ~Channel();

    int fd() const;

    // 设置回调
    void set_read_callback(ReadCallback cb);
    void set_write_callback(WriteCallback cb);

    // 设置感兴趣的事件
    void enable_reading();
    void enable_writing();
    void disable_all();

    // 处理事件
    void handle_event();

    // 更新到epoll
    void update();
};
```

**验收标准**：
- Channel能正确保存fd和事件
- 回调函数能正确调用

---

### 任务3.4：EventLoop类

**目标**：封装epoll，实现单Reactor事件循环

**为什么需要这个组件？**

EventLoop是整个服务器的事件驱动核心，它解决了高性能网络编程的核心问题：

1. **高效处理并发**：传统方式每个连接一个线程，浪费资源；EventLoop单线程处理所有连接
2. **非阻塞I/O**：避免线程在I/O操作上阻塞
3. **事件驱动**：有事情发生时才处理，无事情时休息

**Reactor模式工作原理**：
```
┌──────────────────────────────────────────────────────────────┐
│                         EventLoop                           │
│  ┌────────────┐                                            │
│  │ epoll_wait │←── 等待事件发生                             │
│  └─────┬──────┘                                            │
│        │                                                    │
│        ▼                                                    │
│  ┌────────────┐                                            │
│  │ 处理就绪的  │                                            │
│  │ Channel列表 │                                           │
│  └─────┬──────┘                                            │
│        │                                                    │
│        ▼                                                    │
│  ┌────────────┐                                            │
│  │ 调用回调   │→ handle_read() / handle_write()            │
│  └────────────┘                                            │
└──────────────────────────────────────────────────────────────┘

同时处理：
- listen_socket: 有新连接时触发accept
- client_socket[1]: 有数据可读
- client_socket[2]: 可以写数据
```

**为什么需要wakeup pipe？**
- EventLoop在epoll_wait中阻塞
- 其他线程想让EventLoop处理紧急任务时
- 向pipe写数据，EventLoop立即从epoll_wait返回

**实现要点**：
1. 创建epoll fd（epoll_create1）
2. 实现event_loop()事件循环（epoll_wait）
3. 实现update_channel()添加/更新channel到epoll
4. 实现remove_channel()从epoll删除channel
5. 支持quit()退出循环
6. 创建wakeup pipe支持外部唤醒
7. 实现wakeup()函数

**验收标准**：
- EventLoop能正确注册和监听fd事件
- 事件循环能正确处理I/O事件

**类设计**：
```cpp
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // 禁用拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 事件循环
    void loop();
    void quit();

    // 管理channel
    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    // wake up
    void wakeup();
};
```

**验收标准**：
- EventLoop能正确注册和监听fd事件
- 事件循环能正确处理I/O事件

---

### 任务3.5：Connection类

**目标**：封装一个客户端连接，管理读写缓冲区和Channel

**为什么需要这个组件？**

Connection将一个客户端连接的所有相关资源封装在一起，让代码更清晰：

1. **资源封装**：将Socket、Buffer、Channel打包在一起
2. **读写分离**：input_buffer接收数据，output_buffer发送数据
3. **生命周期管理**：析构时自动关闭连接、释放资源
4. **回调设置**：将消息处理回调传递给Connection

**Connection内部结构**：
```
Connection对象管理的资源：
┌────────────────────────────────────────────┐
│                 Connection                 │
│  ┌─────────────┐  ┌─────────────────┐   │
│  │   Socket    │  │  Channel        │   │
│  │  client_fd  │  │  管理fd事件     │   │
│  └─────────────┘  └─────────────────┘   │
│  ┌─────────────────────────────────────┐  │
│  │  Buffer input_buffer_               │  │  接收客户端数据
│  │  Buffer output_buffer_              │  │  发送给客户端数据
│  └─────────────────────────────────────┘  │
└────────────────────────────────────────────┘
```

**为什么需要两个Buffer？**
- input_buffer：接收网络数据，暂存未处理的命令
- output_buffer：暂存要发送的响应，可能一次发不完

**实现要点**：
1. 保存Socket、Buffer（input/output）、Channel、EventLoop
2. 实现handle_read()：从socket读取数据到input_buffer，调用message_callback
3. 实现handle_write()：从output_buffer发送数据到socket
4. 实现send_response()：追加响应数据到output_buffer，启用写事件
5. 实现close()：从EventLoop移除channel，关闭socket
6. set_message_callback()设置消息回调
7. Connection析构时自动关闭连接

**验收标准**：
- Connection能正确管理客户端连接
- 数据收发功能正常

**类设计**：
```cpp
class Connection {
public:
    Connection(int client_fd, EventLoop* loop);
    ~Connection();

    // 禁用拷贝
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 获取缓冲区
    Buffer* input_buffer();
    Buffer* output_buffer();

    // 设置回调
    void set_message_callback(MessageCallback cb);

    // 处理读写事件
    void handle_read();
    void handle_write();

    // 发送响应
    void send_response(const std::string& resp);

    // 关闭连接
    void close();

    int fd() const;
};
```

**验收标准**：
- Connection能正确管理客户端连接
- 数据收发功能正常

---

## 章节4：协议解析层（第11-13天）

### 任务4.1：RESP协议解析器

**目标**：实现Redis RESP协议的解析和序列化

**为什么需要这个组件？**

RESP（REdis Serialization Protocol）是Redis的通信协议，让ConcurrentCache能够与所有Redis客户端兼容：

1. **兼容性**：所有Redis客户端（redis-cli、jedis、redis-py等）都能直接连接
2. **标准化**：协议简单高效，二进制安全
3. **生态系统**：可以无缝融入现有的Redis生态

**RESP协议格式说明**：

| 类型 | 格式 | 示例 | 说明 |
|-----|------|------|------|
| 简单字符串 | +开头 | +OK\r\n | 成功响应 |
| 错误 | -开头 | -ERR\r\n | 错误响应 |
| 整数 | :开头 | :100\r\n | 整数响应 |
| 批量字符串 | $长度\r\n内容\r\n | $5\r\nhello\r\n | 字符串内容 |
| 数组 | *元素数\r\n | *2\r\n$3\r\nget\r\n | 命令数组 |

**一个完整命令的解析过程**：
```
客户端发送：*2\r\n$3\r\nget\r\n$3\r\nkey\r\n

解析步骤：
1. 读到 *2\r\n → 这是一个2个元素的数组
2. 读取第一个元素：$3\r\nget\r\n → "get"
3. 读取第二个元素：$3\r\nkey\r\n → "key"
4. 结果：["get", "key"]
```

**为什么需要处理粘包？**
- TCP是字节流，可能在任意位置分割数据
- 一次recv()可能收到多个完整命令
- 也可能只收到半个命令
- 解析器需要处理这些边界情况

**实现要点**：
1. 实现RESP协议的5种数据类型解析：简单字符串、错误、整数、批量字符串、数组
2. 实现RESP协议的序列化方法
3. 处理TCP粘包问题（基于\r\n分隔）

**验收标准**：
- `*2\r\n$3\r\nget\r\n$3\r\nkey\r\n` 能正确解析为 `["get", "key"]`
- `+OK\r\n` 能正确序列化
- 能正确处理粘包（多条命令一起发送）

**RESP协议格式**：
```
+简单字符串  : +OK\r\n
-错误        : -ERR\r\n
:整数        : :100\r\n
$批量字符串  : $5\r\nhello\r\n
*数组        : *2\r\n$3\r\nget\r\n$3\r\nkey\r\n
```

**类设计**：
```cpp
class RESPProtocol {
public:
    // 解析：返回解析出的命令数组
    // 输入原始字节数据，返回命令列表
    // 解析失败返回空optional
    static std::optional<std::vector<std::string>> parse(const char* data, size_t len);

    // 序列化方法
    static std::string serialize_simple_string(const std::string& s);  // +OK\r\n
    static std::string serialize_error(const std::string& err);        // -ERR\r\n
    static std::string serialize_integer(long long n);                 // :100\r\n
    static std::string serialize_bulk_string(const std::string& s);   // $5\r\nhello\r\n
    static std::string serialize_null();                               // $-1\r\n

    // 辅助方法
    static std::string ok();       // +OK\r\n
    static std::string error(const std::string& msg);  // -ERR msg\r\n
    static std::string integer(long long n);
    static std::string bulk_string(const std::string& s);
    static std::string null();
};
```

**解析算法要点**：
1. 以 * 开头表示数组，解析数组大小后依次解析每个元素
2. 以 $ 开头表示批量字符串，解析长度后再读取指定字节
3. 以 : 开头表示整数
4. 找到 \r\n 分隔符

**验收标准**：
- `*2\r\n$3\r\nget\r\n$3\r\nkey\r\n` 能正确解析为 `["get", "key"]`
- `+OK\r\n` 能正确序列化
- 能正确处理粘包（多条命令一起发送）

---

## 章节5：命令分发层（第14-16天）

### 任务5.1：命令基类和工厂

**目标**：实现命令路由和分发机制

**为什么需要这个组件？**

命令工厂模式让系统可以方便地添加新命令，是扩展性的基础：

1. **松耦合**：命令的处理逻辑与命令名称解耦
2. **易扩展**：添加新命令只需注册，不需要修改核心代码
3. **统一接口**：所有命令都实现相同的execute()接口
4. **可测试**：每个命令可以独立单元测试

**命令分发流程**：
```
收到命令：["GET", "key1"]

CommandFactory处理流程：
1. factory.create("get") → 返回 GetCommand 对象
2. get_command->execute(args) → "GET key1"
3. 返回响应：bulk_string(value) 或 null

收到未知命令：
1. factory.create("unknown") → 返回 nullptr
2. 返回错误：-ERR unknown command
```

**为什么需要工厂模式？**
- 每次命令执行都创建新的命令对象
- 命令执行完后自动释放
- 不需要预创建所有命令对象

**实现要点**：
1. 定义Command抽象基类，包含execute()纯虚函数
2. 定义CommandFactory命令工厂类（单例）
3. 使用std::unordered_map存储命令名到创建函数的映射
4. register_command()注册命令
5. create()根据命令名创建命令对象
6. exists()判断命令是否存在

**验收标准**：
- 能注册和创建命令对象
- 能根据命令名路由到对应处理函数

**类设计**：
```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual std::string execute(const std::vector<std::string>& args) = 0;
};

class CommandFactory {
public:
    static CommandFactory& instance();

    using Creator = std::function<std::unique_ptr<Command>()>;

    void register_command(const std::string& name, Creator creator);
    std::unique_ptr<Command> create(const std::string& name);
    bool exists(const std::string& name);
};
```

**验收标准**：
- 能注册和创建命令对象
- 能根据命令名路由到对应处理函数

---

### 任务5.2：全局存储

**目标**：实现简单的内存存储（用于骨架版本）

**为什么需要这个组件？**

GlobalStorage是缓存的核心，所有数据都存储在这里。它提供了最基础的键值存储能力：

1. **数据持久化**：数据存储在内存中
2. **快速访问**：O(1)时间复杂度的get/set操作
3. **线程安全**：多线程并发访问时数据不会损坏
4. **简单直接**：对于骨架版本足够简单

**为什么选择unordered_map？**
- O(1)平均时间复杂度
- C++标准库自带，无需额外依赖
- 哈希函数对于字符串key效率高

**为什么要加锁？**
- 多线程可能同时访问同一个key
- 写操作需要互斥保护
- 读操作理论上可以不加锁，但为了简单加写锁

**实现要点**：
1. 使用std::unordered_map存储键值对
2. 提供get()、set()、del()、exists()方法
3. 使用std::shared_mutex保护并发访问
4. 单例模式instance()

**验收标准**：
- 存储和读取数据正确
- 多线程访问安全

**类设计**：
```cpp
class GlobalStorage {
public:
    static GlobalStorage& instance();

    std::string get(const std::string& key);
    void set(const std::string& key, const std::string& value);
    bool del(const std::string& key);
    bool exists(const std::string& key);

private:
    std::unordered_map<std::string, std::string> store_;
    std::shared_mutex mutex_;
};
```

**验收标准**：
- 存储和读取数据正确
- 多线程访问安全

---

### 任务5.3：字符串命令实现

**目标**：实现GET、SET、DEL、EXISTS命令

**为什么需要这些命令？**

GET/SET/DEL/EXISTS是最基础的缓存命令，支撑起了最简单的缓存使用场景：

| 命令 | 作用 | 使用场景 |
|-----|------|---------|
| GET | 获取值 | 读取缓存数据 |
| SET | 设置值 | 写入缓存数据 |
| DEL | 删除键 | 清除缓存数据 |
| EXISTS | 判断存在 | 检查键是否存在 |

**命令使用示例**：
```
# 存储数据
127.0.0.1:6379> SET user:1000:name "张三"
OK

# 读取数据
127.0.0.1:6379> GET user:1000:name
"张三"

# 判断存在
127.0.0.1:6379> EXISTS user:1000:name
(integer) 1

# 删除数据
127.0.0.1:6379> DEL user:1000:name
(integer) 1

# 再次判断
127.0.0.1:6379> EXISTS user:1000:name
(integer) 0
```

**命令处理流程**：
```
SET key value 处理流程：
1. 参数校验：需要3个参数（SET、key、value）
2. 参数不足？返回错误：-ERR wrong number of arguments
3. 调用 GlobalStorage.set(key, value)
4. 返回成功：+OK\r\n

GET key 处理流程：
1. 参数校验：需要2个参数（GET、key）
2. 调用 GlobalStorage.get(key)
3. 值存在？返回 bulk_string(value)
4. 值不存在？返回 null：$-1\r\n
```

**实现要点**：
1. GetCommand：调用GlobalStorage.get()，值不存在返回null
2. SetCommand：调用GlobalStorage.set()，成功返回OK
3. DelCommand：调用GlobalStorage.del()，返回删除数量
4. ExistsCommand：调用GlobalStorage.exists()，返回1或0
5. register_string_commands()注册所有字符串命令到工厂

**验收标准**：
- GET命令：键存在返回值，不存在返回null
- SET命令：存储值并返回OK
- DEL命令：删除成功返回1，键不存在返回0
- EXISTS命令：键存在返回1，不存在返回0

---

## 章节6：服务器入口（第17-18天）

### 任务6.1：主函数整合

**目标**：整合所有模块，实现可运行的服务器

**实现要点**：
1. 初始化日志 init_log()
2. 加载配置文件 Config::instance().load()
3. 初始化信号处理 init_signal()
4. 注册命令 register_string_commands()
5. 创建EventLoop
6. 创建监听Socket，bind_and_listen(port)
7. 设置监听Socket为非阻塞
8. 创建监听Channel，设置读回调（接受连接）
9. 启动事件循环
10. 服务器关闭时输出日志

**主循环逻辑**：
1. is_running()检查退出标志
2. EventLoop.loop()执行事件循环
3. 收到信号时设置退出标志，循环退出

**连接处理逻辑**（读回调中）：
1. accept()接受客户端连接
2. 创建Connection对象
3. 设置message_callback
4. callback中解析RESP命令，路由到对应Command执行，返回响应

**验收标准**：
- 服务器能正常启动
- 能接受客户端连接
- 能处理GET/SET/DEL命令

---

## 章节7：测试与验收（第19-20天）

### 任务7.1：编译测试

**验收标准**：
- `mkdir build && cd build && cmake .. && make -j$(nproc)` 成功编译
- 生成 concurrentcache-server 可执行文件

---

### 任务7.2：功能测试

**验收标准**：
```bash
# 启动服务器
./bin/concurrentcache-server &

# 使用redis-cli测试
redis-cli -p 6379
127.0.0.1:6379> SET key1 value1
OK
127.0.0.1:6379> GET key1
"value1"
127.0.0.1:6379> EXISTS key1
(integer) 1
127.0.0.1:6379> DEL key1
(integer) 1
127.0.0.1:6379> EXISTS key1
(integer) 0

# Ctrl+C 关闭服务器
```

---

### 任务7.3：验收清单

| 功能 | 状态 |
|------|------|
| 服务器启动 | ⬜ |
| 接受客户端连接 | ⬜ |
| SET命令正确存储 | ⬜ |
| GET命令正确返回值 | ⬜ |
| DEL命令正确删除 | ⬜ |
| EXISTS命令正确判断 | ⬜ |
| Ctrl+C优雅退出 | ⬜ |
| 与redis-cli兼容 | ⬜ |

---

## 版本输出

骨架版本完成后的输出：

```
concurrentcache/
├── src/
│   ├── base/
│   │   ├── log.h           # ✅
│   │   ├── config.h        # ✅
│   │   └── signal.h        # ✅
│   ├── network/
│   │   ├── socket.h        # ✅
│   │   ├── buffer.h        # ✅
│   │   ├── channel.h      # ✅
│   │   ├── event_loop.h   # ✅
│   │   ├── event_loop.cpp # ✅
│   │   └── connection.h    # ✅
│   │   └── connection.cpp  # ✅
│   ├── protocol/
│   │   └── resp.h          # ✅
│   ├── command/
│   │   ├── command.h       # ✅
│   │   └── string_cmd.h   # ✅
│   └── server/
│       └── main.cpp        # ✅
├── CMakeLists.txt          # ✅
└── conf/
    └── concurrentcache.conf      # ✅
```

---

## 下一步

骨架版本完成后，可以继续开发 **Version2（基础版本）**：

1. **完善基础工具**：日志文件输出、配置热加载、SIGSEGV处理
2. **添加锁机制**：Mutex、SpinLock、RWLock
3. **实现内存池**：三级分层内存池
4. **实现线程池**：固定任务线程池
5. **升级网络模型**：MainSubReactor多线程模型
6. **添加缓存核心**：过期机制、LRU算法

详见 `Dev.md` 中的 **Version2：基础版本** 部分。
