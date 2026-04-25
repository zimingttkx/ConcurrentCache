// E:/CPPProjects/ConcurrentCache/src/protocol/resp.h
#ifndef CONCURRENTCACHE_PROTOCOL_RESP_H
#define CONCURRENTCACHE_PROTOCOL_RESP_H

// ============================================================================
// 头文件包含说明
// ============================================================================
// #include <string>        - std::string 字符串类
// #include <vector>        - std::vector 动态数组容器
// #include <variant>       - std::variant C++17 联合体，可以存储不同类型的值
// #include <cstdint>       - int64_t 等整数类型的定义
// #include <functional>     - std::function 函数包装器，用于实现回调
// #include "../network/buffer.h" - 引入 Buffer 类，需要知道 buffer 的接口

#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include <functional>
#include "../network/buffer.h"

// 命名空间用于避免全局命名冲突
// 所有 ConcurrentCache 项目的代码都放在 cc_server 命名空间下
// 这样将来如果有其他库也有 RespType 枚举，就不会冲突

namespace cc_server {


// RESP 数据类型枚举
// 枚举（enum）是一组命名整数常量
// enum class 是强类型枚举，比传统 enum 更安全
// 
// RESP 协议用首字符区分数据类型：
//   '+' 开头 = 简单字符串 (Simple String)
//   '-' 开头 = 错误 (Error)
//   ':' 开头 = 整数 (Integer)
//   '$' 开头 = 批量字符串 (Bulk String)
//   '*' 开头 = 数组 (Array)

enum class RespType : char {
    // 每个枚举值后面的 (char) 表示底层类型是 char（1字节字符）
    // 这样做是为了让枚举值可以和字符直接比较（如 data[0] == '+'）
    
    SIMPLE_STRING = '+',  // 简单字符串，例如: +OK\r\n
    ERROR = '-',          // 错误回复，例如: -ERR\r\n
    INTEGER = ':',        // 整数，例如: :100\r\n
    BULK_STRING = '$',    // 批量字符串，例如: $5\r\nhello\r\n
    ARRAY = '*',          // 数组，例如: *2\r\n...\r\n
    UNKNOWN = 0           // 未知类型，用于错误处理
};

// RespValue 结构体
// 结构体用于存储一个解析后的 RESP 值
// 它使用 std::variant 来存储不同类型的值

struct RespValue {
    // --------------------------------------------------------------------
    // std::variant 说明
    // --------------------------------------------------------------------
    // std::variant 是 C++17 引入的"类型安全的联合体"
    // 联合体（union）可以存储不同类型的值，但同一时刻只能存储一种
    // 
    // std::variant 可以包含：
    //   - std::monostate    → 表示"空"或"无值"（类似 null）
    //   - std::string       → 存储字符串（简单字符串、批量字符串的值）
    //   - int64_t           → 存储64位整数（整数的值）
    //   - std::vector<RespValue> → 存储数组（数组的元素列表）
    //
    // 使用 std::get<T>(variant) 可以获取存储的值
    // 使用 std::holds_alternative<T>(variant) 可以检查存储的是什么类型
    
    std::variant<
        std::monostate,              // 第1种可能：空值（表示 null/nil）
        std::string,                  // 第2种可能：字符串（简单字符串或批量字符串）
        int64_t,                     // 第3种可能：64位整数
        std::vector<RespValue>       // 第4种可能：数组（数组元素也是 RespValue）
    > data;
    
    // --------------------------------------------------------------------
    // type 成员变量
    // --------------------------------------------------------------------
    // 记录这个值的 RESP 类型
    // 用于快速判断，不依赖 std::variant 的类型检查
    
    RespType type = RespType::UNKNOWN;
    
    // --------------------------------------------------------------------
    // 便捷方法（Convenience Methods）
    // --------------------------------------------------------------------
    // 这些方法是为了方便获取值，避免每次都写 std::get<>...
    // 方法后面的 const 表示这个方法不会修改对象的成员变量
    
    // 获取字符串值
    // 如果存储的不是字符串，返回空字符串
    [[nodiscard]] std::string as_string() const {
        // std::holds_alternative<T>(variant) 检查 variant 存储的是不是 T 类型
        if (std::holds_alternative<std::string>(data)) {
            // std::get<T>(variant) 获取存储的 T 类型值
            return std::get<std::string>(data);
        }
        return "";  // 不是字符串类型，返回空字符串
    }
    
    // 获取整数值
    // 如果存储的不是整数，返回 0
    [[nodiscard]] int64_t as_integer() const {
        if (std::holds_alternative<int64_t>(data)) {
            return std::get<int64_t>(data);
        }
        return 0;
    }
    
    // 获取数组值
    // 如果存储的不是数组，返回空向量
    // 返回类型是 const 引用，避免复制开销
    [[nodiscard]] const std::vector<RespValue>& as_array() const {
        static std::vector<RespValue> empty;  // 静态空向量，函数结束时不消失
        if (std::holds_alternative<std::vector<RespValue>>(data)) {
            return std::get<std::vector<RespValue>>(data);
        }
        return empty;
    }
    
    // 判断是否为 null
    [[nodiscard]] bool is_null() const {
        return std::holds_alternative<std::monostate>(data);
    }
};

// RespParser 类 - RESP 协议解析器
// 负责从 Buffer 中解析 RESP 协议格式的命令
// 
// 使用方法：
//   1. 创建 RespParser 对象
//   2. 调用 parse(buffer) 方法，传入包含数据的 Buffer
//   3. parse() 返回解析出的所有完整命令（std::vector<RespValue>）
//   4. 已解析的数据会从 Buffer 中自动移除

class RespParser {
public:
    // --------------------------------------------------------------------
    // 构造函数
    // --------------------------------------------------------------------
    RespParser();
    
    // --------------------------------------------------------------------
    // parse() - 核心解析函数
    // --------------------------------------------------------------------
    // 参数：buffer - 指向 Buffer 对象的指针，Buffer 包含要解析的数据
    // 返回：std::vector<RespValue> - 解析出的所有完整命令列表
    //
    // 示例：
    //   Buffer buf;
    //   buf.append("*2\r\n$3\r\nget\r\n$3\r\nkey\r\n", 26);
    //   
    //   RespParser parser;
    //   std::vector<RespValue> commands = parser.parse(&buf);
    //   // commands 现在包含一个数组: ["get", "key"]
    //   // buf 中已解析的数据已被移除
    //
    // 重要：此函数会修改 Buffer 内容，解析完成的数据会通过 buffer->retrieve() 移除
    
    std::vector<RespValue> parse(Buffer* buffer);
    
    // --------------------------------------------------------------------
    // reset() - 重置解析器状态
    // --------------------------------------------------------------------
    // 在解析出错后调用，清除错误信息，恢复初始状态
    // 
    // 示例：
    //   if (!parser.error().empty()) {
    //       parser.reset();  // 清除错误，继续解析
    //   }
    
    void reset();
    
    // --------------------------------------------------------------------
    // error() - 获取错误信息
    // --------------------------------------------------------------------
    // 返回：错误信息字符串，如果解析成功则为空字符串
    // 
    // 使用 const 引用返回，避免复制开销
    
    [[nodiscard]] const std::string& error() const {
        return error_msg_; 
    }
    
    // --------------------------------------------------------------------
    // has_complete_command() - 静态方法，检查是否有完整命令
    // --------------------------------------------------------------------
    // 参数：buffer - 要检查的 Buffer
    // 返回：true 如果 buffer 包含至少一个完整的 RESP 命令
    //       false 如果数据不完整或格式错误
    //
    // 这是一个"静态方法"，属于类而不是对象，可以用 RespParser::has_complete_command() 调用
    // 这个方法不会修改 Buffer，只是检查
    //
    // 使用场景：在循环中判断是否继续解析
    //   while (RespParser::has_complete_command(buffer)) {
    //       // 解析一个命令...
    //   }
    
    static bool has_complete_command(const Buffer* buffer);

private:
    // --------------------------------------------------------------------
    // parse_one() - 解析单个 RESP 值
    // --------------------------------------------------------------------
    // 参数：buffer  - 要解析的 Buffer
    //       out    - 输出参数，解析出的值存入此参数
    // 返回：true 如果解析成功，false 如果数据不完整或格式错误
    //
    // 这个是内部使用的方法，不对外开放
    
    bool parse_one(Buffer* buffer, RespValue& out);
    
    // --------------------------------------------------------------------
    // find_crlf() - 静态方法，查找 \r\n 位置
    // --------------------------------------------------------------------
    // 参数：data - 要搜索的数据起始指针
    //       len  - 要搜索的字节数
    // 返回：指向找到的 \r\n 位置的指针，如果没找到返回 nullptr
    //
    // \r\n 是 RESP 协议的分隔符（CRLF = Carriage Return + Line Feed）
    // 例如 "OK\r\n" 中，\r\n 在位置 2-3
    
    static const char* find_crlf(const char* data, size_t len);
    
    // --------------------------------------------------------------------
    // 各类型的解析方法
    // --------------------------------------------------------------------
    // 每个方法负责解析特定类型的 RESP 数据
    // out 是输出参数，存储解析结果
    // 返回 true 表示成功，false 表示失败
    
    bool parse_simple_string(Buffer* buffer, std::string& out);
    bool parse_error(Buffer* buffer, std::string& out);
    bool parse_integer(Buffer* buffer, int64_t& out);
    bool parse_bulk_string(Buffer* buffer, std::string& out);
    bool parse_array(Buffer* buffer, std::vector<RespValue>& out);
    
    // --------------------------------------------------------------------
    // skip() - 静态方法，跳过指定字节数
    // --------------------------------------------------------------------
    // 调用 buffer->retrieve(len) 来丢弃已读取的数据
    
    static void skip(Buffer* buffer, size_t len);

    // --------------------------------------------------------------------
    // error_msg_ - 错误信息成员变量
    // --------------------------------------------------------------------
    // 存储最近的错误信息
    
    std::string error_msg_;
    
    // --------------------------------------------------------------------
    // CR 和 LF - 常量
    // --------------------------------------------------------------------
    // CR = Carriage Return，回车符，ASCII 13，'\r'
    // LF = Line Feed，换行符，ASCII 10，'\n'
    // 它们组合在一起 "\r\n" 是 RESP 协议的行分隔符
    
    static constexpr char CR = '\r';  // constexpr 表示编译时常量
    static constexpr char LF = '\n';
};

// RespEncoder 类 - RESP 协议编码器
// 负责将数据编码为 RESP 协议格式
// 与 RespParser 相反：RespParser 把 RESP 字符串解析成数据，Encoder 把数据编码成 RESP 字符串

class RespEncoder {
public:
    // --------------------------------------------------------------------
    // encode_simple_string() - 编码简单字符串
    // --------------------------------------------------------------------
    // 参数：s - 要编码的字符串
    // 返回：RESP 格式的简单字符串
    //
    // RESP 简单字符串格式: +<content>\r\n
    // 示例: encode_simple_string("OK") 返回 "+OK\r\n"
    
    static std::string encode_simple_string(const std::string& s);
    
    // --------------------------------------------------------------------
    // encode_error() - 编码错误消息
    // --------------------------------------------------------------------
    // 参数：err - 错误消息内容
    // 返回：RESP 格式的错误消息
    //
    // RESP 错误格式: -<content>\r\n
    // 但 Redis 实际格式是: -ERR <message>\r\n
    // 所以这个函数会自动加上 "ERR " 前缀
    // 示例: encode_error("unknown command") 返回 "-ERR unknown command\r\n"
    
    static std::string encode_error(const std::string& err);
    
    // --------------------------------------------------------------------
    // encode_integer() - 编码整数（64位版本）
    // --------------------------------------------------------------------
    // 参数：n - 要编码的整数
    // 返回：RESP 格式的整数
    //
    // RESP 整数格式: :<number>\r\n
    // 示例: encode_integer(100) 返回 ":100\r\n"
    
    static std::string encode_integer(int64_t n);
    
    // --------------------------------------------------------------------
    // encode_integer() - 编码整数（int 版本，兼容性重载）
    // --------------------------------------------------------------------
    // 参数：n - 要编码的整数
    // 返回：RESP 格式的整数
    //
    // 这是上面那个函数的 int 版本重载
    // C++ 允许函数名相同但参数不同（函数重载）
    
    static std::string encode_integer(int n);
    
    // --------------------------------------------------------------------
    // encode_bulk_string() - 编码批量字符串
    // --------------------------------------------------------------------
    // 参数：s - 要编码的字符串
    // 返回：RESP 格式的批量字符串
    //
    // RESP 批量字符串格式: $<length>\r\n<content>\r\n
    // 示例: encode_bulk_string("hello") 返回 "$5\r\nhello\r\n"
    //   ($5 表示后面有5个字节)
    
    static std::string encode_bulk_string(const std::string& s);
    
    // --------------------------------------------------------------------
    // encode_null() - 编码 null 值
    // --------------------------------------------------------------------
    // 返回：RESP 格式的 null
    //
    // RESP null bulk string 格式: $-1\r\n
    // 表示一个不存在的值（类似其他语言的 null/nil）
    
    static std::string encode_null();
    
    // --------------------------------------------------------------------
    // encode_array() - 编码字符串数组
    // --------------------------------------------------------------------
    // 参数：arr - 要编码的字符串向量
    // 返回：RESP 格式的数组
    //
    // RESP 数组格式: *<count>\r\n$<len1>\r\n<content1>\r\n...
    // 示例: encode_array({"get", "key"}) 返回:
    //   "*2\r\n$3\r\nget\r\n$3\r\nkey\r\n"
    
    static std::string encode_array(const std::vector<std::string>& arr);
    
    // --------------------------------------------------------------------
    // encode_ok() - 便捷方法，编码 OK 响应
    // --------------------------------------------------------------------
    // 等同于 encode_simple_string("OK")
    // 返回: "+OK\r\n"
    
    static std::string encode_ok();
    
    // --------------------------------------------------------------------
    // encode_nil() - 便捷方法，编码 nil 响应
    // --------------------------------------------------------------------
    // 等同于 encode_null()
    // 返回: "$-1\r\n"
    
    static std::string encode_nil();
};

}  // namespace cc_server

#endif  // CONCURRENTCACHE_PROTOCOL_RESP_H
