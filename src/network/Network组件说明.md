# network 目录组件详解

本文档详细解释 `src/network` 目录下五个网络组件的设计思想、实现原理和协作关系，帮助初学者理解高性能网络编程的核心概念。

---

## 目录

1. [Socket (socket.h / socket.cpp)](#1-socket-socketh-socketcpp)
2. [Buffer (buffer.h / buffer.cpp)](#2-buffer-bufferh-buffercpp)
3. [Channel (channel.h / channel.cpp)](#3-channel-channelh-channelcpp)
4. [EventLoop (event_loop.h / event_loop.cpp)](#4-eventloop-event_looph-event_loopcpp)
5. [Connection (connection.h / connection.cpp)](#5-connection-connectionh-connectioncpp)
6. [组件协作流程](#6-组件协作流程)
7. [常见问题解答](#7-常见问题解答)

---

## 1. Socket (socket.h / socket.cpp)

### 1.1 什么是 Socket？

Socket 是 Linux 网络编程的核心概念，可以理解为"网络文件的门票"。当程序想通过网络通信时，需要先创建一个 Socket，操作系统会分配一个文件描述符（fd）来代表这个网络连接。

**类比理解**：
- 想象你要打电话，先需要一个电话号码
- Socket 就相当于这个电话号码
- 文件描述符 fd 就是操作系统给你的"电话号码牌"

### 1.2 核心 API

```cpp
socket(AF_INET, SOCK_STREAM, 0);  // 创建 TCP 套接字
bind(socket_fd, address, port);   // 绑定地址和端口
listen(socket_fd, backlog);       // 开始监听连接
accept(socket_fd);                // 接受客户端连接
send(socket_fd, data, len);       // 发送数据
recv(socket_fd, buf, len);        // 接收数据
close(socket_fd);                 // 关闭连接
```

### 1.3 非阻塞模式

**问题**：普通 socket 的 `accept()`、`recv()`、`send()` 可能会阻塞等待。

**解决**：使用 `SOCK_NONBLOCK` 标志，设置为非阻塞模式。

```cpp
// 创建非阻塞套接字
int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

// 非阻塞 accept：没有连接时立即返回 -1，errno = EAGAIN
int client_fd = accept4(fd, ... , SOCK_NONBLOCK);
```

**非阻塞的好处**：
- 不会卡住主线程
- 配合 epoll 使用，实现高性能事件驱动

### 1.4 地址复用 (SO_REUSEADDR)

```cpp
int opt = 1;
setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

**问题**：服务器重启时，端口可能处于 TIME_WAIT 状态，立即重启会失败。

**解决**：开启 SO_REUSEADDR，允许绑定处于 TIME_WAIT 的端口。

### 1.5 Socket 类的 RAII 设计

```cpp
class Socket {
private:
    int fd_;
public:
    ~Socket() {
        close();  // 析构时自动关闭
    }
};
```

**为什么这样设计**？
- 不需要手动调用 `close()`
- 对象离开作用域时自动关闭套接字
- 防止资源泄漏

### 1.6 移动语义

```cpp
// 移动构造
Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;  // 原对象不再拥有 fd
}

// 移动赋值
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();      // 先关闭自己的资源
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}
```

**为什么需要移动语义**？
- 避免复制文件描述符（复制后两个对象指向同一 fd，关闭时二次关闭会出错）
- 提高性能（直接转移所有权，不需要复制）

### 1.7 使用示例

```cpp
// 创建服务器监听套接字
Socket server;
server.bind_and_listen(6379);

// 接受客户端连接
int client_fd = server.accept();

// 发送数据
const char* msg = "Hello";
server.send(msg, strlen(msg));

// 关闭（析构自动调用 close()）
```

---

## 2. Buffer (buffer.h / buffer.cpp)

### 2.1 为什么需要缓冲区？

**TCP 粘包问题**：
- TCP 是"流协议"，数据像水管里的水，没有固定边界
- 你发了 "Hello" 和 "World"，对方可能收到 "HelloWorld"
- 或者 "Hel" + "loWorld"，不确定在哪里分割

**Buffer 的作用**：
- 缓存已收到但还没处理完的数据
- 提供明确的读写接口，屏蔽 TCP 流协议的细节

### 2.2 双指针模型

Buffer 内部使用两个指针（索引）管理数据：

```
┌──────────────────────────────────────────────────────────┐
│  buffer_ (vector<char>)                                 │
├──────────────────────────────────────────────────────────┤
│  [ reader_idx_ |← readable →| ← writable →|            │
└──────────────────────────────────────────────────────────┘
         ↑                  ↑                  ↑
      reader_idx_       writer_idx_         capacity

readable_bytes() = writer_idx_ - reader_idx_
writable_bytes() = buffer_.size() - writer_idx_
```

### 2.3 写数据流程

```cpp
void Buffer::append(const char* data, size_t len) {
    ensure_writable(len);  // 确保空间足够（不够自动扩容）
    std::copy(data, data + len, buffer_.begin() + writer_idx_);
    writer_idx_ += len;    // 移动写指针
}
```

### 2.4 读数据流程

```cpp
// 查看数据（不移动指针）
const char* Buffer::peek() const {
    return buffer_.data() + reader_idx_;
}

// 标记已读（移动读指针）
void Buffer::retrieve(size_t len) {
    if (len > readable_bytes()) {
        reader_idx_ = writer_idx_;  // 读太多，直接到末尾
    } else {
        reader_idx_ += len;          // 正常移动
    }
}
```

**为什么不删除数据而移动指针？**
- 删除数据需要搬移内存，O(n) 复杂度
- 移动指针是 O(1)
- 后续通过 `compact()` 压缩空间

### 2.5 自动扩容

```cpp
void Buffer::ensure_writable(size_t len) {
    if (writable_bytes() < len) {
        // 空间不够，扩容到 writer_idx_ + len
        buffer_.resize(writer_idx_ + len);
    }
}
```

### 2.6 空间压缩 (compact)

**问题**：读指针不断后移，前面空间浪费了。

**解决**：当 reader_idx_ 超过缓冲区一半时，压缩空间。

```cpp
void Buffer::compact() {
    if (reader_idx_ == 0) return;
    std::copy(
        buffer_.begin() + reader_idx_,    // 从有效数据开始
        buffer_.begin() + writer_idx_,    // 到有效数据结束
        buffer_.begin()                   // 拷贝到起始位置
    );
    writer_idx_ -= reader_idx_;  // 调整写指针
    reader_idx_ = 0;             // 读指针归零
}
```

### 2.7 使用示例

```cpp
Buffer buf;

// 写入数据
buf.append("Hello", 5);        // buf: "Hello"
buf.append("World", 5);       // buf: "HelloWorld"

// 读取数据
size_t len = buf.readable_bytes();  // 10
const char* data = buf.peek();      // "HelloWorld"
buf.retrieve(5);                    // 标记已读5字节

// 转为字符串
std::string s = buf.to_string();     // "World"
```

---

## 3. Channel (channel.h / channel.cpp)

### 3.1 什么是 Channel？

Channel 是"文件描述符 + 事件监听 + 回调函数"的封装。

**解决的问题**：
- 一个 fd 上可能发生多种事件（可读、可写、错误）
- 每种事件需要不同的处理逻辑
- Channel 把这些绑定在一起

### 3.2 回调函数机制

```cpp
using ReadCallback  = std::function<void()>;
using WriteCallback = std::function<void()>;
using ErrorCallback = std::function<void()>;

Channel ch;
ch.set_read_callback([]() { /* 处理可读事件 */ });
ch.set_write_callback([]() { /* 处理可写事件 */ });
ch.set_error_callback([]() { /* 处理错误事件 */ });
```

**为什么要用 `std::function`？**
- 普通函数指针只能绑全局函数或静态成员函数
- `std::function` 可以存储 lambda、bind 表达式、任意可调用对象
- 非常灵活，适合事件驱动编程

### 3.3 事件监听

```cpp
// 启用读事件（EPOLLIN）
void Channel::enable_reading() {
    events_ |= EPOLLIN;  // 位或运算添加事件
    update();            // 注册到 epoll
}

// 启用写事件（EPOLLOUT）
void Channel::enable_writing() {
    events_ |= EPOLLOUT;
    update();
}

// 禁用所有事件
void Channel::disable_all() {
    events_ = 0;
    update();
}
```

**EPOLLIN / EPOLLOUT 是什么意思？**
- EPOLLIN： socket 可读（数据到达或对方关闭连接）
- EPOLLOUT： socket 可写（内核发送缓冲区有空位）
- EPOLLERR： 错误发生
- EPOLLHUP： 对方关闭连接（挂起）

### 3.4 事件分发

```cpp
void Channel::handle_event() {
    // 可读事件
    if (triggered_events_ & EPOLLIN) {
        if (read_cb_) read_cb_();
    }
    // 可写事件
    if (triggered_events_ & EPOLLOUT) {
        if (write_cb_) write_cb_();
    }
    // 错误事件
    if (triggered_events_ & (EPOLLERR | EPOLLHUP)) {
        if (error_cb_) error_cb_();
    }
}
```

**为什么要判断每种事件？**
- epoll_wait 可能返回多种事件
- 可能是多个事件同时触发
- 需要分别处理

### 3.5 update() 的作用

```cpp
void Channel::update() {
    loop_->update_channel(this);
}
```

Channel 本身不直接操作 epoll，而是交给 EventLoop 处理。这是职责分离的设计。

### 3.6 使用示例

```cpp
Channel* ch = new Channel(loop, fd);

ch->set_read_callback([this]() {
    this->handle_read();
});

ch->set_write_callback([this]() {
    this->handle_write();
});

ch->enable_reading();  // 开始监听读事件
```

---

## 4. EventLoop (event_loop.h / event_loop.cpp)

### 4.1 什么是 EventLoop？

EventLoop（事件循环）是整个服务器的核心引擎，基于 epoll 实现事件驱动。

**工作原理**：
```
主循环：
    1. 调用 epoll_wait() 等待事件
    2. 有事件就绪时，遍历所有就绪的 fd
    3. 找到对应的 Channel，调用其 handle_event()
    4. 回到步骤 1
```

### 4.2 epoll 基础

**为什么不用 select/poll？**
- select 有 fd 数量限制（最多 1024）
- poll 没有数量限制，但需要遍历所有 fd
- epoll 是 Linux 最优解，无需遍历，直接返回就绪的 fd

**epoll 三步曲**：
```cpp
// 1. 创建 epoll 实例
int epoll_fd = epoll_create1(0);

// 2. 注册要监听的 fd 和事件
struct epoll_event ev;
ev.events = EPOLLIN;       // 监听可读
ev.data.fd = fd;
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

// 3. 等待事件发生
struct epoll_event events[1024];
int n = epoll_wait(epoll_fd, events, 1024, -1);  // -1 永久阻塞
```

### 4.3 Channel 映射表

```cpp
std::unordered_map<int, Channel*> channels_;
```

**为什么需要这个映射表？**
- epoll_wait 只返回"哪个 fd 就绪了"
- 但处理事件需要调用对应的 Channel 对象
- 映射表实现 fd → Channel* 的快速查找

**时间复杂度**：
- unordered_map 查找是 O(1)
- 即使有几万个连接，查找也很快

### 4.4 wakeup pipe（跨线程唤醒）

**问题**：EventLoop 在 epoll_wait 中阻塞，如何让其他线程叫醒它？

**解决**：创建一个 pipe，其他线程往写端写数据，EventLoop 读端就会触发可读事件。

```
┌─────────────┐         pipe        ┌─────────────┐
│ 其他线程    │ ────────────────▶  │ EventLoop   │
│ (写端)      │   'a' (一个字节)    │ (读端)      │
└─────────────┘                    └─────────────┘
```

**使用场景**：
- 其他线程想让主线程处理紧急任务
- quit() 被调用，需要 EventLoop 立即退出

### 4.5 主循环实现

```cpp
void EventLoop::loop() {
    while (!quit_) {
        int n = epoll_wait(
            epoll_fd_,
            events_.data(),
            static_cast<int>(events_.size()),
            100  // 超时 100ms
        );

        for (int i = 0; i < n; ++i) {
            int fd = events_[i].data.fd;

            // 如果是 wakeup pipe
            if (fd == wakeup_fd_) {
                handle_wakeup();
                continue;
            }

            // 找对应的 Channel
            auto it = channels_.find(fd);
            if (it != channels_.end()) {
                it->second->set_triggered_events(events_[i].events);
                it->second->handle_event();
            }
        }
    }
}
```

### 4.6 Channel 管理

```cpp
void EventLoop::update_channel(Channel* channel) {
    int fd = channel->fd();

    auto it = channels_.find(fd);
    if (it == channels_.end()) {
        // 新增：ADD
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        channels_[fd] = channel;
    } else {
        // 已存在：MOD
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    }
}
```

### 4.7 优雅退出

```cpp
void EventLoop::quit() {
    quit_ = true;   // 设置退出标志
    wakeup();       // 唤醒 epoll_wait，让它及时退出
}
```

---

## 5. Connection (connection.h / connection.cpp)

### 5.1 什么是 Connection？

Connection 是"一个客户端连接"的管理器，它整合了网络层的所有组件。

**包含的组件**：
```cpp
class Connection {
    Socket client_socket_;     // 客户端套接字
    EventLoop* loop_;          // 所属事件循环
    Channel* channel_;         // 事件通道
    Buffer input_buffer_;      // 接收缓冲区
    Buffer output_buffer_;     // 发送缓冲区
};
```

### 5.2 构造函数

```cpp
Connection::Connection(int client_fd, EventLoop* loop)
    : client_socket_(client_fd),
      loop_(loop),
      channel_(nullptr),
      input_buffer_(),
      output_buffer_()
{
    // 创建 Channel
    channel_ = new Channel(loop_, client_socket_.fd());

    // 设置回调
    channel_->set_read_callback([this]() { this->handle_read(); });
    channel_->set_write_callback([this]() { this->handle_write(); });
    channel_->set_error_callback([this]() { this->close(); });

    // 开始监听读事件
    channel_->enable_reading();
}
```

### 5.3 handle_read() 详解

```cpp
void Connection::handle_read() {
    char temp_buffer[4096];
    ssize_t bytes_read = client_socket_.recv(
        temp_buffer,
        sizeof(temp_buffer) - 1
    );

    if (bytes_read > 0) {
        // 成功读到数据，加入输入缓冲区
        input_buffer_.append(temp_buffer, bytes_read);

    } else if (bytes_read == 0) {
        // 对方关闭连接
        close();

    } else {
        // 读失败
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // 正常，没数据了
        }
        close();  // 出错，关闭连接
    }
}
```

### 5.4 handle_write() 详解

```cpp
void Connection::handle_write() {
    // 检查有没有数据要发
    if (output_buffer_.readable_bytes() == 0) {
        channel_->disable_all();
        channel_->enable_reading();
        return;
    }

    // 发送数据
    const char* data = output_buffer_.peek();
    size_t len = output_buffer_.readable_bytes();
    ssize_t bytes_written = client_socket_.send(data, len);

    if (bytes_written > 0) {
        output_buffer_.retrieve(bytes_written);  // 移动读指针

        // 发完了？
        if (output_buffer_.readable_bytes() == 0) {
            channel_->disable_all();
            channel_->enable_reading();
        }
    }
}
```

### 5.5 send_response() 详解

**为什么需要这个函数而不是直接 send()？**

```cpp
void Connection::send_response(const char* data, size_t len) {
    output_buffer_.append(data, len);  // 先放入缓冲区
    channel_->enable_writing();        // 启用写事件
}
```

**问题**：直接 send() 可能因为内核缓冲区满而阻塞或返回部分发送。

**解决**：放入 output_buffer_，等 epoll 通知"可写"时再发，handle_write() 会自动续传。

### 5.6 生命周期管理

```cpp
Connection::~Connection() {
    if (channel_ != nullptr) {
        loop_->remove_channel(channel_);  // 先从 epoll 移除
        delete channel_;                  // 再删除 Channel
    }
    // Socket 析构会自动 close()
}
```

**为什么要先 remove_channel？**
- 如果不先移除，epoll 还监听已关闭的 fd
- 可能触发无效的事件通知

---

## 6. 组件协作流程

### 6.1 服务器启动流程

```
Server Main
     │
     ▼
┌─────────────┐
│   Socket    │  bind_and_listen(6379)
│ (监听套接字) │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  EventLoop  │  创建 epoll 实例
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  Channel    │  监听 socket 的可读事件
│ (accept 等待)│
└─────────────┘
       │
       ▼
EventLoop::loop() 开始循环
```

### 6.2 客户端连接流程

```
客户端 connect()
        │
        ▼ TCP 三次握手
┌─────────────────────────────────────┐
│        EventLoop::loop()            │
│   epoll_wait 检测到 socket 可读      │
│                                     │
│   socket->accept()                  │
│           │                         │
│           ▼                         │
│   ┌──────────────┐                  │
│   │  Connection  │  新建连接对象    │
│   │  (客户端)    │                  │
│   └──────┬───────┘                  │
│          │                          │
│          ▼                          │
│   ┌──────────────┐                  │
│   │    Channel   │  监听客户端 fd   │
│   │  (客户端)    │                  │
│   └──────────────┘                  │
└─────────────────────────────────────┘
```

### 6.3 数据收发流程

```
┌─────────────────────────────────────────────────────────┐
│                    接收数据流程                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  客户端发送数据                                          │
│         │                                               │
│         ▼ TCP 数据到达内核缓冲区                         │
│  ┌─────────────┐                                       │
│  │    socket   │  recv() 从内核复制到用户态              │
│  │   (recv)    │                                       │
│  └──────┬──────┘                                       │
│         │                                               │
│         ▼                                               │
│  ┌─────────────┐                                       │
│  │input_buffer │  缓存数据                             │
│  │   append()  │                                       │
│  └─────────────┘                                       │
│         │                                               │
│         ▼ 业务层处理                                    │
│  ┌─────────────┐                                       │
│  │   业务逻辑   │  从 input_buffer 读取并处理           │
│  └─────────────┘                                       │
│                                                         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                    发送数据流程                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  业务层处理完成                                          │
│         │                                               │
│         ▼                                               │
│  ┌─────────────┐                                       │
│  │output_buffer│  send_response() 数据放入发送缓冲区    │
│  │   append()  │                                       │
│  └──────┬──────┘                                       │
│         │                                               │
│         ▼                                               │
│  ┌─────────────┐                                       │
│  │   Channel   │  enable_writing() 启用写事件           │
│  └──────┬──────┘                                       │
│         │                                               │
│         ▼ epoll 通知可写                                │
│  ┌─────────────┐                                       │
│  │handle_write │  从 output_buffer 取出数据             │
│  └──────┬──────┘                                       │
│         │                                               │
│         ▼                                               │
│  ┌─────────────┐                                       │
│  │   socket    │  send() 复制到内核发送缓冲区           │
│  │   (send)    │                                       │
│  └──────┬──────┘                                       │
│         │                                               │
│         ▼ TCP 传输                                      │
│  客户端收到数据                                          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 6.4 完整请求处理流程

```
redis-cli 发送: SET key value
        │
        ▼
┌───────────────────┐
│   Socket 监听     │  端口 6379
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  EventLoop        │  epoll_wait 等待
│  accept()         │
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│   Connection      │  新建客户端连接
│  handle_read()    │
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  input_buffer     │  缓存 "SET key value\r\n"
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  RESP 协议解析    │  解析命令（后续章节）
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  CommandFactory    │  创建 SET 命令
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  GlobalStorage     │  执行存储
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  output_buffer    │  放入 "+OK\r\n"
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  Channel          │  enable_writing()
│  handle_write()   │
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│   Socket          │  send() 发送
└─────────┬─────────┘
          │
          ▼
redis-cli 收到: +OK
```

### 6.5 组件依赖关系

```
┌─────────────────────────────────────────────────────────────┐
│                      组件依赖图                              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   ┌─────────────┐                                          │
│   │   Logger    │  ← 基础依赖（日志输出）                    │
│   └──────┬──────┘                                          │
│          │                                                  │
│   ┌──────▼──────┐                                          │
│   │   Socket    │  ← 封装 socket API                       │
│   └──────┬──────┘                                          │
│          │                                                  │
│   ┌──────▼──────┐                                          │
│   │   Buffer    │  ← Socket 依赖（数据读写）                 │
│   └─────────────┘                                          │
│                                                             │
│   ┌─────────────┐      ┌─────────────┐                     │
│   │   Socket    │ ───▶ │   Channel   │                     │
│   └─────────────┘      └──────┬──────┘                     │
│                               │                             │
│                        ┌──────▼──────┐                      │
│                        │  EventLoop  │                      │
│                        └──────┬──────┘                      │
│                               │                             │
│                        ┌──────▼──────┐                      │
│                        │ Connection │                      │
│                        └─────────────┘                      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. 常见问题解答

### Q: 为什么要用非阻塞 socket？

A：阻塞模式下，如果当前没有数据，`recv()` 会一直等待，导致整个线程卡住。非阻塞模式下，没有数据立即返回，让出 CPU 给其他任务处理。

### Q: epoll 相比 select/poll 有什么优势？

A：
| 特性 | select | poll | epoll |
|------|--------|------|-------|
| fd 数量限制 | 1024 | 无限制 | 无限制 |
| 时间复杂度 | O(n) | O(n) | O(1) |
| 工作方式 | 遍历所有 fd | 遍历所有 fd | 只返回就绪的 fd |
| 内核数据结构 | bitmap | 数组 | 红黑树 + 就绪链表 |

### Q: 什么是 ET（边缘触发）和 LT（水平触发）？

A：
- **LT（水平触发）**：只要条件满足就一直通知。适合初学者。
- **ET（边缘触发）**：只在状态变化时通知一次。性能更高，但需要一次性处理所有就绪数据。

本项目使用 LT（默认模式），简单可靠。

### Q: Buffer 的 writer_idx_ 会不会无限增长？

A：不会。Buffer 使用 `std::vector<char>` 存储，`resize()` 会自动扩容。读指针移动后可以通过 `compact()` 压缩空间，释放前端空闲内存。

### Q: Connection 析构时为什么要先 remove_channel？

A：因为 epoll 还在监听这个 fd，如果不先移除，epoll 可能向已删除的 Channel 发送事件，导致未定义行为。

### Q: `std::function` 和函数指针有什么区别？

A：
```cpp
// 函数指针：只能指向全局函数或静态函数
void (*callback)() = &my_function;

// std::function：可以存储任意可调用对象
std::function<void()> callback = []() { LOG_INFO("hello"); };
callback = &my_function;        // 也可以是函数指针
callback = std::bind(&MyClass::method, &obj);  // 也可以是成员函数
```

### Q: 为什么 send_response() 要先放入缓冲区？

A：直接 send() 可能因为内核缓冲区满而阻塞或返回部分发送。放入 output_buffer_ 后，等 epoll 通知"可写"时再发，handle_write() 会自动续传，确保数据完整发送。

### Q: 如何处理高并发连接？

A：本项目是单 Reactor 模式，适合低并发场景（< 10000 连接）。高并发需要：
1. 多 Reactor 模式（主从 EventLoop）
2. 线程池处理业务逻辑
3. 使用 `EPOLLET` 边缘触发模式

---

## 8. 代码逻辑流程图

### EventLoop 事件循环

```
开始
  │
  ▼
epoll_create1() 创建 epoll 实例
  │
  ▼
create_wakeup_pipe() 创建唤醒管道
  │
  ▼
注册 wakeup_fd 到 epoll
  │
  ▼
while (!quit_)
  │
  ├──▶ epoll_wait() 阻塞等待 100ms
  │        │
  │        ▼
  │   有事件就绪？
  │    │
  │    ├── 否（超时）──▶ 继续等待
  │    │
  │    └── 是
  │        │
  │        ▼
  │   遍历所有就绪事件
  │    │
  │    ▼
  │   判断事件类型
  │    │
  │    ├── wakeup_fd ──▶ handle_wakeup()
  │    │
  │    └── 普通 fd ──▶ 查找 Channel
  │                      │
  │                      ▼
  │                   handle_event()
  │                      │
  │                      ▼
  │                   触发对应回调
  │
  └── quit_ == true？──▶ 退出循环
        │
        ▼
结束
```

### Connection 数据收发

```
┌─────────────────────────────────────────────────────────┐
│                    读数据流程                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  handle_read() 被调用                                    │
│         │                                               │
│         ▼                                               │
│  socket.recv() 从内核读取数据                            │
│         │                                               │
│         ▼                                               │
│  bytes_read > 0？                                        │
│    │                                                    │
│    ├── 是                                               │
│    │    │                                               │
│    │    ▼                                               │
│    │   input_buffer_.append() 数据放入缓冲区            │
│    │    │                                               │
│    │    ▼                                               │
│    │   业务层处理（后续）                               │
│    │                                                    │
│    └── 否（bytes_read == 0）                           │
│         │                                               │
│         ▼                                               │
│        对方关闭 close()                                  │
│                                                         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                    写数据流程                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  send_response(data, len) 被调用                        │
│         │                                               │
│         ▼                                               │
│  output_buffer_.append() 数据放入缓冲区                  │
│         │                                               │
│         ▼                                               │
│  channel_->enable_writing() 启用写事件                   │
│         │                                               │
│         ▼                                               │
│  epoll_wait 返回 socket 可写                             │
│         │                                               │
│         ▼                                               │
│  handle_write() 被调用                                  │
│         │                                               │
│         ▼                                               │
│  output_buffer_.readable_bytes() == 0？                 │
│    │                                                    │
│    ├── 是：禁用写事件，只保留读事件                       │
│    │                                                    │
│    └── 否                                               │
│         │                                               │
│         ▼                                               │
│    socket.send() 发送数据                                │
│         │                                               │
│         ▼                                               │
│    output_buffer_.retrieve() 移动读指针                  │
│         │                                               │
│         ▼                                               │
│    还需要继续发送？──▶ 启用写事件，等待下次               │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

---

## 9. 进一步学习建议

1. **Linux 网络编程**：
   - 《Linux 高性能服务器编程》游静等
   - 《Unix 网络编程》Stevens 著

2. **epoll 深入理解**：
   - `man epoll` 查看官方文档
   - 研究 nginx 的事件驱动架构

3. **C++ 并发编程**：
   - 《C++ Concurrency in Action》Anthony Williams
   - 重点理解 mutex、condition_variable、atomic

4. **Reactor 模式**：
   - 研究 libevent、libuv 的实现
   - 理解单 Reactor vs 多 Reactor 的权衡

5. **项目实践**：
   - 尝试添加一个 DEL 命令
   - 研究 Redis 6.0 的多线程 IO
   - 实现简单的连接超时关闭
