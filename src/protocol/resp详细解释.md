# RESP 协议解析器详解

## 目录

1. [我的项目是做什么的？](#1-我的项目是做什么的)
2. [什么是 RESP 协议？](#2-什么是-resp-协议)
3. [RespParser 是什么？](#3-respparser-是什么)
4. [RespEncoder 是什么？](#4-respencoder-是什么)
5. [这些组件在项目中的位置](#5-这些组件在项目中的位置)
6. [数据流转全过程](#6-数据流转全过程)
7. [为什么需要这两个组件？](#7-为什么需要这两个组件)
8. [与其他组件的关系](#8-与其他组件的关系)

---

## 1. 我的项目是做什么的？

**ConcurrentCache** 是一个仿 Redis 的缓存服务器。

简单来说：
- 你可以通过 `redis-cli` 客户端连接到这个服务器
- 你可以执行 `SET key value`（存储数据）
- 你可以执行 `GET key`（获取数据）
- 你可以执行 `DEL key`（删除数据）

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│                    ConcurrentCache 服务器                     │
│                         (我的项目)                           │
│                                                             │
│    监听端口 6379                                            │
│    接收命令，执行命令，返回结果                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                           ▲
                           │ 连接
                           │
              ┌────────────────────────┐
              │     redis-cli         │
              │    (Redis 客户端)      │
              └────────────────────────┘
```

---

## 2. 什么是 RESP 协议？

### 2.1 协议是什么？

**协议 = 通信双方约定的数据格式**

就像：
- 中国人和中国人说话，用中文（协议）
- 中国人和美国人说话，用英文（协议）
- 你的程序和 redis-cli 通信，用 RESP 协议

### 2.2 RESP 协议格式

RESP（REdis Serialization Protocol）是 Redis 官方制定的通信协议。

当你在 redis-cli 输入命令时：

```
redis-cli> SET name "zhangsan"
OK
```

redis-cli **不会**直接把文字发给服务器，而是把命令**转换成 RESP 格式**发送：

### 2.3 RESP 五种数据类型

| 类型 | 首字符 | 格式 | 示例 |
|------|--------|------|------|
| 简单字符串 | `+` | `+内容\r\n` | `+OK\r\n` |
| 错误 | `-` | `-内容\r\n` | `-ERR unknown command\r\n` |
| 整数 | `:` | `:数字\r\n` | `:100\r\n` |
| 批量字符串 | `$` | `$长度\r\n内容\r\n` | `$8\r\nzhangsan\r\n` |
| 数组 | `*` | `*元素数量\r\n` | `*3\r\n...` |

### 2.4 命令的 RESP 格式

你在 redis-cli 输入：
```
SET name "zhangsan"
```

redis-cli 转换成 RESP 发送：
```
*3\r\n               ← 数组，有 3 个元素
$3\r\n               ← 第一个元素：批量字符串，长度 3
SET\r\n              ← 内容是 "SET"
$4\r\n               ← 第二个元素：批量字符串，长度 4
name\r\n             ← 内容是 "name"
$8\r\n               ← 第三个元素：批量字符串，长度 8
zhangsan\r\n         ← 内容是 "zhangsan"
```

**完整写法：**
```
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhangsan\r\n
```

### 2.5 \r\n 是什么？

`\r\n` 是两个字符：
- `\r` = 回车符（Carriage Return），ASCII 13
- `\n` = 换行符（Line Feed），ASCII 10

它们组合在一起 `\r\n` 表示**一行的结束**，类似于按回车键。

RESP 协议用 `\r\n` 分隔不同的数据。

---

## 3. RespParser 是什么？

### 3.1 作用

**RespParser = RESP 协议的解析器**

它的任务：
- **输入**：一段 RESP 格式的原始数据（字节流）
- **输出**：程序员容易使用的数据结构

### 3.2 举个例子

**输入**（从网络收到的原始数据）：
```
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhangsan\r\n
```

**输出**（解析后的结果）：
```cpp
["SET", "name", "zhangsan"]
```

这就是 RespParser.parse() 做的事。

### 3.3 为什么叫"解析"？

"解析"类似于"翻译"：
- 你不懂法语，有人帮你翻译成中文
- 你不懂 RESP 格式，RespParser 帮你翻译成 C++ 的数据结构

### 3.4 C++ 数据结构设计

我们定义了一个 `RespValue` 结构体来存储解析结果：

```cpp
struct RespValue {
    // variant 可以存储不同类型的值，类似"万能盒子"
    std::variant<
        std::monostate,              // 空值（类似 null）
        std::string,                  // 字符串
        int64_t,                     // 整数
        std::vector<RespValue>       // 数组
    > data;

    RespType type;  // 记录数据类型
};
```

**为什么用 variant？**

因为 RESP 有5种数据类型，但 C++ 要求变量有固定类型。

`std::variant` 就像一个"多变盒子"：
- 这一刻装字符串
- 下一刻装整数
- 再下一刻装数组

### 3.5 RespType 枚举

```cpp
enum class RespType : char {
    SIMPLE_STRING = '+',  // 简单字符串：+OK\r\n
    ERROR = '-',          // 错误：-ERR\r\n
    INTEGER = ':',        // 整数：:100\r\n
    BULK_STRING = '$',    // 批量字符串：$5\r\nhello\r\n
    ARRAY = '*',          // 数组：*2\r\n...
    UNKNOWN = 0           // 未知类型
};
```

枚举就像给数字起名字：
- `SIMPLE_STRING` 的实际值是 `'+'`
- 这样代码更易读

### 3.6 解析函数 parse()

```cpp
class RespParser {
public:
    // 解析函数
    // 参数：buffer - 包含原始数据的缓冲区
    // 返回：vector<RespValue> - 解析出的所有命令列表
    std::vector<RespValue> parse(Buffer* buffer);
};
```

**使用示例：**

```cpp
// 假设 Buffer 中有数据 "*2\r\n$3\r\nget\r\n$3\r\nkey\r\n"

RespParser parser;
std::vector<RespValue> commands = parser.parse(buffer);

// commands 现在包含解析结果
// commands[0] 是一个数组类型的 RespValue
// 数组第一个元素是 "get"
// 数组第二个元素是 "key"
```

### 3.7 为什么解析后数据从 Buffer 中消失？

解析完成后，`parse()` 会调用 `buffer->retrieve()` **移除已解析的数据**。

这是因为：
- Buffer 容量有限
- 已解析的数据不再需要
- 留着会占用空间

```
解析前 Buffer:  "*2\r\n$3\r\nget\r\n$3\r\nkey\r\n"
解析后 Buffer:  ""  （数据已被移除）
```

### 3.8 粘包问题

TCP 是"流式协议"，数据像水流一样连续不断。

**问题：** 一次 `recv()` 可能收到：
- 半个命令（数据不完整）
- 一个完整命令
- 多个命令（粘在一起）

**解决方案：** `parse()` 用循环处理

```cpp
while (has_complete_command(buffer)) {
    // 只要有完整命令，就继续解析
    commands.push_back(parse_one(buffer));
}
```

`has_complete_command()` 检查是否有完整命令：
- 有 → 解析一个
- 没有 → 退出循环，等待更多数据

---

## 4. RespEncoder 是什么？

### 4.1 作用

**RespEncoder = RESP 协议的编码器**

它的任务：
- **输入**：程序中的数据（如字符串、整数）
- **输出**：RESP 格式的字节流（发给客户端）

### 4.2 举个例子

**输入**（程序中的数据）：
```cpp
"OK"
```

**输出**（RESP 格式）：
```
+OK\r\n
```

这就是 `RespEncoder::encode_simple_string("OK")` 做的事。

### 4.3 为什么要"编码"？

因为客户端只认 RESP 格式！

你程序里存的是普通字符串 `"OK"`，但发给 redis-cli 必须转换成 `+OK\r\n`。

### 4.4 编码函数列表

```cpp
class RespEncoder {
public:
    static std::string encode_simple_string("OK");       // → "+OK\r\n"
    static std::string encode_error("ERR");              // → "-ERR ERR\r\n"
    static std::string encode_integer(100);              // → ":100\r\n"
    static std::string encode_bulk_string("hello");      // → "$5\r\nhello\r\n"
    static std::string encode_null();                     // → "$-1\r\n"
    static std::string encode_ok();                      // → "+OK\r\n"
};
```

### 4.5 使用示例

```cpp
// 服务器要返回 "OK" 给客户端
std::string resp = RespEncoder::encode_ok();
// resp = "+OK\r\n"

// 服务器要返回 "hello" 给客户端
std::string resp = RespEncoder::encode_bulk_string("hello");
// resp = "$5\r\nhello\r\n"

// 服务器要返回空值（null）
std::string resp = RespEncoder::encode_null();
// resp = "$-1\r\n"
```

---

## 5. 这些组件在项目中的位置

### 5.1 项目架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         完整请求处理流程                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   redis-cli  ──────►  TCP 数据流  ──────►  服务器                    │
│                                                                     │
│   输入命令:                                                          │
│   SET name zhangsan                                                 │
│                                                                     │
│   实际发送:                                                         │
│   *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhangsan\r\n              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                          网络层 (已完成)                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌──────────┐    ┌──────────┐    ┌──────────┐                     │
│   │  Socket  │───►│  Buffer  │───►│ Channel  │───►  EventLoop      │
│   │  监听    │    │  读写缓冲 │    │  事件封装 │                     │
│   └──────────┘    └──────────┘    └──────────┘                     │
│                                                                     │
│   ┌──────────┐                                                      │
│   │Connection│  handle_read() 从 socket 读取数据到 Buffer            │
│   │  连接管理 │  handle_write() 从 Buffer 发送数据到 socket           │
│   └──────────┘                                                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ Buffer 中有原始数据
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         协议解析层 (当前开发)                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌──────────────┐    ┌──────────────┐                              │
│   │ RespParser   │    │ RespEncoder  │                              │
│   │              │    │              │                              │
│   │ 解析请求      │    │ 编码响应      │                              │
│   │ 输入 ──► 输出 │    │ 输入 ──► 输出 │                              │
│   │              │    │              │                              │
│   │ *3\r\n...   │    │ "OK"        │                              │
│   │     ↓       │    │     ↓       │                              │
│   │ ["SET",     │    │ "+OK\r\n"   │                              │
│   │  "name",   │    │             │                              │
│   │  "zhang"] │    │             │                              │
│   └──────────────┘    └──────────────┘                              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ 解析后的命令数组
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                          命令分发层 (待开发)                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌──────────────────┐                                              │
│   │ CommandFactory   │  命令工厂                                     │
│   │                  │                                              │
│   │ 根据命令名创建    │                                              │
│   │ 对应的命令处理器  │                                              │
│   └────────┬─────────┘                                              │
│            │                                                        │
│   ┌────────┴────────┬────────────┐                                  │
│   ▼                 ▼            ▼                                  │
│ ┌──────┐      ┌──────┐     ┌──────┐                                │
│ │ GET  │      │ SET  │     │ DEL  │                                │
│ │Command│      │Command│     │Command│                                │
│ └──────┘      └──────┘     └──────┘                                │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 组件职责总结

| 组件 | 职责 | 输入 | 输出 |
|------|------|------|------|
| **Socket** | 网络通信基础 | 无 | 无 |
| **Buffer** | 管理读写缓冲区 | 原始字节 | 原始字节 |
| **Channel** | 管理文件描述符事件 | 无 | 无 |
| **EventLoop** | 事件循环 | 无 | 无 |
| **Connection** | 管理客户端连接 | socket 数据 | socket 数据 |
| **RespParser** | 解析 RESP 协议 | 原始字节 | 命令数组 |
| **RespEncoder** | 编码 RESP 协议 | 程序数据 | RESP 格式字节 |

---

## 6. 数据流转全过程

### 6.1 客户端发送命令

```
redis-cli 输入: SET name "zhangsan"

redis-cli 转换为 RESP:
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhangsan\r\n
              │
              ▼ TCP 发送
┌─────────────────────────────────────────────────────┐
│ 服务器                                                    │
│                                                     │
│ Socket 接收数据                                        │
│              │                                        │
│              ▼                                        │
│ ┌─────────────────────────────────┐                  │
│ │ Buffer input_buffer_            │                  │
│ │ "*3\r\n$3\r\nSET\r\n..."       │  ← 原始数据      │
│ └─────────────────────────────────┘                  │
│              │                                        │
│              ▼                                        │
│ ┌─────────────────────────────────┐                  │
│ │ RespParser.parse() 解析          │                  │
│ │ 输入: "*3\r\n..."               │                  │
│ │ 输出: ["SET", "name", "zhang"]  │  ← 程序员能用的  │
│ └─────────────────────────────────┘                  │
│              │                                        │
│              ▼                                        │
│ ┌─────────────────────────────────┐                  │
│ │ 命令分发 (CommandFactory)        │                  │
│ │ 创建 SetCommand                 │                  │
│ │ 调用 execute(["SET", ...])      │                  │
│ └─────────────────────────────────┘                  │
│              │                                        │
│              ▼                                        │
│ ┌─────────────────────────────────┐                  │
│ │ 存储到 GlobalStorage             │                  │
│ │ hash_map["name"] = "zhang"      │                  │
│ └─────────────────────────────────┘                  │
│              │                                        │
│              ▼                                        │
│ ┌─────────────────────────────────┐                  │
│ │ RespEncoder.encode_ok() 编码     │                  │
│ │ 输入: "OK"                       │                  │
│ │ 输出: "+OK\r\n"                  │                  │
│ └─────────────────────────────────┘                  │
│              │                                        │
│              ▼                                        │
│ Buffer output_buffer_ = "+OK\r\n"                    │
│              │                                        │
│              ▼                                        │
│ Socket 发送数据到客户端                                 │
│              │                                        │
              ▼
客户端收到: +OK\r\n
redis-cli 显示: OK
```

### 6.2 关键函数调用

```cpp
// 1. Connection::handle_read() - 接收数据
char temp_buffer[4096];
ssize_t bytes_read = socket.recv(temp_buffer, ...);
input_buffer_.append(temp_buffer, bytes_read);

// 2. RespParser::parse() - 解析协议
std::vector<RespValue> commands = parser.parse(input_buffer());

// 3. 命令处理
for (auto& cmd : commands) {
    const auto& arr = cmd.as_array();  // 获取命令数组
    std::string cmd_name = arr[0].as_string();  // "SET"
    std::string key = arr[1].as_string();  // "name"
    std::string value = arr[2].as_string();  // "zhangsan"

    // 执行 SET 命令
    storage.set(key, value);
}

// 4. RespEncoder::encode_ok() - 编码响应
std::string response = RespEncoder::encode_ok();
// response = "+OK\r\n"

// 5. Connection::send_response() - 发送响应
send_response(response);
socket.send(response.data(), response.size());
```

---

## 7. 为什么需要这两个组件？

### 7.1 RespParser 的必要性

**问题：** 客户端发来的数据是 RESP 格式的字节流，程序看不懂。

**RespParser 解决：** 把字节流翻译成程序员容易处理的数据结构。

```
不用 RespParser（困难）:
  程序要自己解析 "*3\r\n$3\r\nSET\r\n..."，理解 \r\n、$、* 等符号

用 RespParser（简单）:
  程序直接用 commands[0].as_array()["SET", "name", "zhangsan"]
```

### 7.2 RespEncoder 的必要性

**问题：** 程序中的数据（如字符串 "OK"）不是 RESP 格式，客户端不认识。

**RespEncoder 解决：** 把程序数据转换成 RESP 格式的字节流。

```
不用 RespEncoder（困难）:
  程序要自己拼接 "+OK\r\n"，注意 \r\n 的位置

用 RespEncoder（简单）:
  程序直接用 RespEncoder::encode_ok() 得到 "+OK\r\n"
```

### 7.3 它们是配套的

| 组件 | 方向 | 转换 |
|------|------|------|
| **RespParser** | RESP → 程序数据 | 解析请求 |
| **RespEncoder** | 程序数据 → RESP | 编码响应 |

```
请求处理:  网络数据 → RespParser → 程序数据
响应处理:  程序数据 → RespEncoder → 网络数据
```

---

## 8. 与其他组件的关系

### 8.1 和 Buffer 的关系

```
┌─────────────────────────────────────┐
│            Buffer                   │
│                                     │
│  ┌───────────────────────────────┐  │
│  │  reader_idx_ │ 可读数据 │ 可写 │  │
│  └───────────────────────────────┘  │
│                ↑                    │
│                │ peek()             │
│                │ retrieve()         │
│                │ append()           │
└─────────────────────────────────────┘
                    │
                    │ 指针传递
                    ▼
┌─────────────────────────────────────┐
│          RespParser                 │
│                                     │
│  parse(buffer)                      │
│       │                             │
│       └──► 读取 buffer->peek()      │
│            处理数据                 │
│            调用 buffer->retrieve()   │
└─────────────────────────────────────┘
```

**注意：** RespParser **不拥有** Buffer，只是借用它来读取数据。

### 8.2 和 Connection 的关系

RespParser 可以集成到 Connection，也可以独立使用。

**方式一：Connection 调用 RespParser（我建议的）**

```cpp
class Connection {
private:
    RespParser resp_parser_;  // 持有解析器
    CommandCallback callback_;  // 命令回调

public:
    void handle_read() {
        // 从 socket 读取到 buffer
        socket.recv(buffer);

        // 解析命令
        auto commands = resp_parser_.parse(buffer);

        // 调用回调处理
        for (auto& cmd : commands) {
            callback_(cmd, this);
        }
    }
};
```

**方式二：外部调用 RespParser（Connection 保持纯净）**

```cpp
// Connection 不变，只是提供数据访问接口

class Connection {
public:
    Buffer* input_buffer() { return &input_buffer_; }
    Buffer* output_buffer() { return &output_buffer_; }
};

// 外部代码自己调用
void on_message(Connection* conn) {
    auto commands = parser.parse(conn->input_buffer());
    // 处理命令...
}
```

**两种方式的区别：**

| 方式 | 优点 | 缺点 |
|------|------|------|
| 方式一 | 代码内聚，Connection 知道协议解析 | Connection 职责变多 |
| 方式二 | Connection 保持简洁 | 外部需要知道协议解析 |

### 8.3 和 CommandFactory 的关系

```
RespParser 解析出命令数组
        │
        │ ["SET", "name", "zhangsan"]
        ▼
┌───────────────────────────────────────┐
│         CommandFactory                  │
│                                       │
│  create("SET") ──► 返回 SetCommand    │
│                                       │
│  ["SET", "name", "zhangsan"]         │
│              │                        │
│              ▼                        │
│  SetCommand.execute(args)             │
│              │                        │
│              ▼                        │
│  存储数据，返回 "+OK\r\n"              │
└───────────────────────────────────────┘
```

---

## 附录：常见问题

### Q1: 为什么协议首字符是 +, -, :, $, *？

这是 RESP 协议设计者选的符号。这些符号在正常文本内容中很少出现，可以作为类型的明确标识。

### Q2: $ 和 * 后面为什么是数字？

- `$5` 表示后续内容是 5 个字节
- `*3` 表示数组有 3 个元素

这样解析器就知道要读取多少数据。

### Q3: 为什么需要 \r\n？

`\r\n` 是行分隔符。RESP 用它来标记数据的边界，让解析器知道一行到哪里结束。

### Q4: $-1 表示什么？

`-1` 在批量字符串中表示 `null`（空值）。因为 `$0\r\n\r\n` 表示空字符串，而 `null` 需要用 `-1` 区分。

### Q5: 粘包是什么意思？

TCP 是流式协议，不保留消息边界。一次 recv() 可能收到：
- 半个命令（数据不完整）
- 一个完整命令
- 多个命令粘在一起

`parse()` 用循环处理这种情况。
