// E:/CPPProjects/ConcurrentCache/src/protocol/resp.cpp
#include "resp.h"
#include <sstream>
#include "../base/log.h"

namespace cc_server {

// RespParser 构造函数
RespParser::RespParser() {
    // 调用 reset() 初始化解析器状态
    reset();
}

// reset() - 重置解析器状态
void RespParser::reset() {
    // 清空错误信息
    error_msg_.clear();

    // 注意：这里没有重置其他成员变量
    // 因为每次调用 parse() 都会完整解析，不存在中间状态
    // 如果解析失败，整个 parse() 调用会失败，不存在"恢复后继续"的需求
}


// parse() - 核心解析函数
std::vector<RespValue> RespParser::parse(Buffer* buffer) {

    std::vector<RespValue> commands;  // 存储解析出的命令列表

    // --------------------------------------------------------------------
    // 解析循环
    // --------------------------------------------------------------------
    // while 循环：条件为真时持续执行循环体
    //
    // has_complete_command(buffer) 检查 buffer 中是否有完整命令
    // 如果有，返回 true，继续解析
    // 如果没有（数据不完整），返回 false，退出循环

    while (has_complete_command(buffer)) {
        // ----------------------------------------------------------------
        // RespValue cmd - 存储单个解析结果
        // ----------------------------------------------------------------
        // 创建空的 RespValue 对象，用于接收解析结果

        RespValue cmd;

        // ----------------------------------------------------------------
        // parse_one() - 解析一个完整的 RESP 值
        // ----------------------------------------------------------------
        // 参数：
        //   buffer - 要解析的 Buffer（指针）
        //   cmd    - 输出参数，解析结果存入此对象
        //
        // 返回值：
        //   true  - 解析成功
        //   false - 解析失败（可能是数据格式错误）

        if (parse_one(buffer, cmd)) {
            // 解析成功，将结果添加到 commands 向量
            // push_back() 在向量末尾添加元素
            commands.push_back(cmd);
        } else {
            // 解析失败，可能是数据不完整或格式错误
            // 退出循环，保留未解析的数据在 buffer 中
            break;
        }
    }

    // --------------------------------------------------------------------
    // 返回解析结果
    // --------------------------------------------------------------------
    // 如果收到多个命令（如 "GET k1\r\nGET k2\r\n"），会全部解析并返回
    // 如果数据不完整，返回空的 vector

    return commands;
}

// ============================================================================
// has_complete_command() - 静态方法，检查是否有完整命令
// ============================================================================
//
// 重要：这是一个"静态方法"，属于类而不是对象
// 调用方式：RespParser::has_complete_command(buffer)
//
// 这个方法的设计理念是"快速检查，不执行实际解析"
// 它只扫描数据查找 \r\n 分隔符，判断是否有完整命令
//
// 这样设计的好处：
// 1. 避免在数据不完整时执行部分解析
// 2. 可以快速判断是否应该继续循环
// 3. 性能高效

bool RespParser::has_complete_command(const Buffer* buffer) {
    // --------------------------------------------------------------------
    // peek() 和 readable_bytes() - Buffer 类的方法
    // --------------------------------------------------------------------
    // peek() - 返回指向可读数据起始位置的指针（不移动读取位置）
    // readable_bytes() - 返回可读的字节数
    //
    // 注意：buffer 是 const 指针（指向 const Buffer）
    // 这意味着我们只能读取数据，不能修改

    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    // --------------------------------------------------------------------
    // 快速失败：数据太短不可能是完整命令
    // --------------------------------------------------------------------
    // RESP 最短命令是简单字符串 "+OK\r\n" = 5 字节
    // 如果数据少于 4 字节，肯定不是完整命令
    // （虽然理论上 +OK\r\n 是 5 字节，但加上这个检查可以避免边界问题）

    if (len < 4) return false;

    // --------------------------------------------------------------------
    // 获取数据类型标识
    // --------------------------------------------------------------------
    // data[0] 是第一个字节，即 RESP 类型的首字符
    // '+' = 简单字符串
    // '-' = 错误
    // ':' = 整数
    // '$' = 批量字符串
    // '*' = 数组

    char type = data[0];

    // --------------------------------------------------------------------
    // switch 语句 - 多条件分支
    // --------------------------------------------------------------------
    // switch (变量) 根据变量的值跳转到不同的 case
    // 类似于多个 if-else 但更清晰

    switch (type) {
        // ----------------------------------------------------------------
        // 简单字符串、错误、整数：格式都是 <type><content>\r\n
        // 只需要找到 \r\n 即可判断完整性
        // ----------------------------------------------------------------
        case '+':  // 简单字符串: +OK\r\n
        case '-':  // 错误: -ERR\r\n
        case ':':  // 整数: :100\r\n

            // 在 data + 1 位置开始搜索，跳过首字符
            // len - 1 是剩余字节数
            // find_crlf() 返回指向 \r\n 的指针
            if (const char* crlf = find_crlf(data + 1, len - 1)) {
                // 找到了 \r\n，说明有完整行
                return true;
            }
            return false;  // 没找到，数据不完整

        // ----------------------------------------------------------------
        // 批量字符串: $<len>\r\n<content>\r\n
        // 需要先解析长度，再检查内容是否完整
        // ----------------------------------------------------------------
        case '$': {
            // 查找长度行的 \r\n（"$<len>\r\n"）
            const char* crlf = find_crlf(data + 1, len - 1);
            if (!crlf) {
                // 没找到，字符串长度行不完整
                return false;
            }

            // ----------------------------------------------------------------
            // 解析长度
            // ----------------------------------------------------------------
            // data + 1 指向长度的第一个数字
            // crlf 指向 \r\n 的 \r
            // 所以长度字符串是 [data+1, crlf)

            std::string len_str(data + 1, crlf - (data + 1));

            // std::stoll() - "String TO Long Long"
            // 将字符串转换为 64 位整数
            // 例如: "5" -> 5, "100" -> 100

            int64_t bulk_len = std::stoll(len_str);

            // ----------------------------------------------------------------
            // 处理 null bulk string
            // ----------------------------------------------------------------
            // $-1\r\n 表示 null，长度为 -1
            // 这种情况下命令已经完整（只需要长度行 + \r\n）

            if (bulk_len < 0) {
                // 检查是否有足够的字节容纳 $-1\r\n
                // 即: 当前位置 + 4 字节 (\r\n + $-1\r\n)
                // 实际上 len >= 5 已经足够
                return len >= 5 && crlf[1] == '\n';
            }

            // ----------------------------------------------------------------
            // 计算完整批量字符串的总长度
            // ----------------------------------------------------------------
            // $<len>\r\n = (crlf - data) + 2 字节
            // <content>\r\n = bulk_len + 2 字节

            size_t header_len = (crlf - data) + 2;  // "$<len>\r\n" 的长度
            size_t total_len = header_len + bulk_len + 2;  // 完整命令长度

            return len >= total_len;  // 比较数据长度是否足够
        }

        // ----------------------------------------------------------------
        // 数组: *<count>\r\n<elements>
        // 需要解析元素个数，然后递归检查每个元素
        // ----------------------------------------------------------------
        case '*': {
            // 查找数组计数的 \r\n
            const char* crlf = find_crlf(data + 1, len - 1);
            if (!crlf) return false;

            // 解析数组元素个数
            std::string count_str(data + 1, crlf - (data + 1));
            int64_t count = std::stoll(count_str);

            // 空数组（count = 0）或 null 数组（count < 0）
            // 只需要 *<count>\r\n 就是完整的
            if (count <= 0) {
                return true;
            }

            // ----------------------------------------------------------------
            // 检查数组元素是否完整
            // ----------------------------------------------------------------
            // 跳过 *<count>\r\n，开始检查每个元素
            size_t pos = (crlf - data) + 2;

            for (int64_t i = 0; i < count; ++i) {
                // 如果已经到达数据末尾，说明最后一个元素不完整
                if (pos >= len) return false;

                char elem_type = data[pos];

                // 根据元素类型，跳过相应的字节数
                // 然后检查下一个元素

                if (elem_type == '+' || elem_type == '-' || elem_type == ':') {
                    // 简单字符串/错误/整数: 格式 <type><content>\r\n
                    // 跳过类型字符和内容（找到下一个 \r\n）
                    const char* elem_crlf = find_crlf(data + pos + 1, len - pos - 1);
                    if (!elem_crlf) return false;
                    pos = (elem_crlf - data) + 2;

                } else if (elem_type == '$') {
                    // 批量字符串: $<len>\r\n<content>\r\n
                    const char* elem_crlf = find_crlf(data + pos + 1, len - pos - 1);
                    if (!elem_crlf) return false;

                    std::string elem_len_str(data + pos + 1, elem_crlf - (data + pos + 1));
                    int64_t elem_len = std::stoll(elem_len_str);

                    if (elem_len < 0) {
                        // null bulk: $-1\r\n
                        pos = (elem_crlf - data) + 2;
                    } else {
                        size_t elem_total_len = (elem_crlf - data) + 2 + elem_len + 2;
                        if (len < elem_total_len) return false;
                        pos = elem_total_len;
                    }

                } else if (elem_type == '*') {
                    // 嵌套数组: 简化处理
                    // 找到匹配的结束位置
                    int depth = 1;
                    size_t start = pos;
                    while (depth > 0 && pos < len) {
                        if (data[pos] == '*') depth++;
                        if (data[pos] == '\n' && pos > 0 && data[pos-1] == '\r') {
                            depth--;
                        }
                        pos++;
                    }
                    if (depth != 0) return false;

                } else {
                    // 未知类型
                    return false;
                }
            }

            return true;
        }

        // ----------------------------------------------------------------
        // 默认情况：首字符不是有效的 RESP 类型
        // ----------------------------------------------------------------
        default:
            return false;
    }
}

// ============================================================================
// find_crlf() - 静态方法，查找 \r\n 位置
// ============================================================================

const char* RespParser::find_crlf(const char* data, size_t len) {
    // --------------------------------------------------------------------
    // 函数返回类型：const char*
    // --------------------------------------------------------------------
    // const char* 是"指向 const char 的指针"
    // 返回的指针指向找到的 \r\n 位置（指向 \r 字符）
    // 如果没找到，返回 nullptr

    // --------------------------------------------------------------------
    // for 循环
    // --------------------------------------------------------------------
    // for (初始化; 条件; 更新)
    // 循环变量 i 从 0 开始，每次增加 1，直到 i + 1 >= len（即没有下一个字符对）

    for (size_t i = 0; i + 1 < len; ++i) {
        // ----------------------------------------------------------------
        // 检查当前字符和下一个字符
        // ----------------------------------------------------------------
        // data[i]     是当前字符（可能是 \r）
        // data[i + 1] 是下一个字符（可能是 \n）
        //
        // 如果当前是 \r 且下一个是 \n，说明找到了 CRLF

        if (data[i] == CR && data[i + 1] == LF) {
            // 返回指向 \r 的指针
            return data + i;
        }
    }

    // 循环结束，没找到
    return nullptr;
}

// ============================================================================
// skip() - 静态方法，跳过指定字节数
// ============================================================================

void RespParser::skip(Buffer* buffer, size_t len) {
    // --------------------------------------------------------------------
    // 这个方法是对 Buffer::retrieve() 的包装
    // retrieve() 会移动读取指针，标记数据为"已读"
    // 已读的数据可以被覆盖或清理
    // --------------------------------------------------------------------

    buffer->retrieve(len);
}

// ============================================================================
// parse_one() - 解析单个 RESP 值
// ============================================================================

bool RespParser::parse_one(Buffer* buffer, RespValue& out) {
    // --------------------------------------------------------------------
    // 获取 Buffer 中的数据（但不移动读取位置）
    // peek() 返回指向可读数据的指针
    // readable_bytes() 返回可读的字节数
    // --------------------------------------------------------------------

    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    // --------------------------------------------------------------------
    // 空数据检查
    // --------------------------------------------------------------------
    // 如果没有数据可读，返回 false
    // 这种情况不应该发生，因为调用 parse_one 之前会检查 has_complete_command

    if (len < 1) {
        error_msg_ = "Buffer is empty";
        return false;
    }

    // --------------------------------------------------------------------
    // 获取数据类型标识
    // --------------------------------------------------------------------

    char type = data[0];

    // --------------------------------------------------------------------
    // switch 分支处理不同类型
    // --------------------------------------------------------------------

    switch (type) {
        case '+': {
            // 简单字符串
            std::string content;
            if (!parse_simple_string(buffer, content)) return false;
            out.type = RespType::SIMPLE_STRING;
            out.data = content;
            return true;
        }

        case '-': {
            // 错误
            std::string content;
            if (!parse_error(buffer, content)) return false;
            out.type = RespType::ERROR;
            out.data = content;
            return true;
        }

        case ':': {
            // 整数
            int64_t value = 0;
            if (!parse_integer(buffer, value)) return false;
            out.type = RespType::INTEGER;
            out.data = value;
            return true;
        }

        case '$': {
            // 批量字符串
            std::string content;
            if (!parse_bulk_string(buffer, content)) return false;
            out.type = RespType::BULK_STRING;
            out.data = content;
            return true;
        }

        case '*': {
            // 数组
            std::vector<RespValue> arr;
            if (!parse_array(buffer, arr)) return false;
            out.type = RespType::ARRAY;
            out.data = arr;
            return true;
        }

        default: {
            // 未知类型
            error_msg_ = "Unknown RESP type: " + std::string(1, type);
            return false;
        }
    }
}

// ============================================================================
// parse_simple_string() - 解析简单字符串
// ============================================================================
// 格式: +<content>\r\n
// 例如: +OK\r\n, +Hello\r\n

bool RespParser::parse_simple_string(Buffer* buffer, std::string& out) {
    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    // 查找 \r\n 分隔符
    const char* crlf = find_crlf(data + 1, len - 1);
    if (!crlf) {
        error_msg_ = "Simple string missing CRLF";
        return false;
    }

    // ----------------------------------------------------------------
    // 提取字符串内容
    // ----------------------------------------------------------------
    // data + 1 跳过首字符 '+'
    // crlf - (data + 1) 是内容的长度
    // std::string(a, n) 构造一个字符串，从 a 开始 n 个字符

    out = std::string(data + 1, crlf - (data + 1));

    // ----------------------------------------------------------------
    // 跳过已解析的数据
    // ----------------------------------------------------------------
    // (crlf - data) + 2 = 跳到 \r 的位置 + 2 = 跳过整个命令
    //
    // 例如 "+OK\r\n":
    //   data 指向 '+'
    //   crlf - data = 2 (指向 'O' 的位置，但实际是 '\r' 位置减 data)
    //   等等，让我重新计算...
    //   "+OK\r\n"
    //    ^  ^  ^ ^
    //    0  1  2 3 4  (位置)
    //   data[0] = '+'
    //   data[1] = 'O'
    //   data[2] = 'K'
    //   data[3] = '\r'
    //   data[4] = '\n'
    //
    //   crlf = data + 3 (指向 '\r')
    //   crlf - data = 3
    //   (crlf - data) + 2 = 5，正好跳过全部 5 个字节

    skip(buffer, (crlf - data) + 2);
    return true;
}

// ============================================================================
// parse_error() - 解析错误
// ============================================================================
// 格式: -<content>\r\n
// 例如: -ERR unknown command\r\n

bool RespParser::parse_error(Buffer* buffer, std::string& out) {
    // 错误解析和简单字符串解析完全相同
    // 只是语义不同（表示错误信息）

    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    const char* crlf = find_crlf(data + 1, len - 1);
    if (!crlf) {
        error_msg_ = "Error message missing CRLF";
        return false;
    }

    out = std::string(data + 1, crlf - (data + 1));
    skip(buffer, (crlf - data) + 2);
    return true;
}

// ============================================================================
// parse_integer() - 解析整数
// ============================================================================
// 格式: :<digits>\r\n
// 例如: :100\r\n

bool RespParser::parse_integer(Buffer* buffer, int64_t& out) {
    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    const char* crlf = find_crlf(data + 1, len - 1);
    if (!crlf) {
        error_msg_ = "Integer missing CRLF";
        return false;
    }

    // ----------------------------------------------------------------
    // 提取数字字符串并转换
    // ----------------------------------------------------------------
    // std::string(a, n) 从 a 开始 n 个字符
    // std::stoll() 将字符串转换为 long long (64位整数)

    std::string num_str(data + 1, crlf - (data + 1));

    try {
        // ----------------------------------------------------------------
        // std::stoll() 可能抛出异常
        // ----------------------------------------------------------------
        // 如果字符串不是有效的数字（如 "abc"），会抛出 std::invalid_argument
        // 如果数字超出范围，会抛出 std::out_of_range
        //
        // try-catch 是 C++ 的异常处理机制

        out = std::stoll(num_str);
    } catch (...) {
        // catch (...) 捕获所有异常
        error_msg_ = "Invalid integer: " + num_str;
        return false;
    }

    skip(buffer, (crlf - data) + 2);
    return true;
}

// ============================================================================
// parse_bulk_string() - 解析批量字符串
// ============================================================================
// 格式: $<length>\r\n<content>\r\n
// 例如: $5\r\nhello\r\n
//       $11\r\nhello world\r\n

bool RespParser::parse_bulk_string(Buffer* buffer, std::string& out) {
    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    // ----------------------------------------------------------------
    // 第1步：解析长度行
    // ----------------------------------------------------------------

    const char* crlf = find_crlf(data + 1, len - 1);
    if (!crlf) {
        error_msg_ = "Bulk string missing length CRLF";
        return false;
    }

    // 提取长度字符串
    std::string len_str(data + 1, crlf - (data + 1));
    int64_t bulk_len = std::stoll(len_str);

    // 计算头部长度（"$<len>\r\n"）
    size_t header_len = (crlf - data) + 2;

    // ----------------------------------------------------------------
    // 第2步：处理 null bulk string
    // ----------------------------------------------------------------
    // $-1\r\n 表示 null（空值）
    // 这种情况下，命令已经完整，不需要内容部分

    if (bulk_len < 0) {
        skip(buffer, header_len);  // 只跳过 $-1\r\n
        out = "";  // 返回空字符串，表示 null
        return true;
    }

    // ----------------------------------------------------------------
    // 第3步：检查内容是否完整
    // ----------------------------------------------------------------
    // 完整长度 = 头部长度 + 内容长度 + \r\n (2字节)

    size_t total_len = header_len + bulk_len + 2;

    if (len < total_len) {
        error_msg_ = "Bulk string incomplete";
        return false;
    }

    // ----------------------------------------------------------------
    // 第4步：提取内容
    // ----------------------------------------------------------------
    // 内容从 header_len 位置开始，长度为 bulk_len

    out = std::string(data + header_len, bulk_len);

    // ----------------------------------------------------------------
    // 第5步：跳过已解析的数据
    // ----------------------------------------------------------------
    // 跳过整个命令，包括结尾的 \r\n

    skip(buffer, total_len);
    return true;
}

// ============================================================================
// parse_array() - 解析数组
// ============================================================================
// 格式: *<count>\r\n<elements>
// 例如: *2\r\n$3\r\nget\r\n$3\r\nkey\r\n
//       *3\r\n:1\r\n:2\r\n:3\r\n

bool RespParser::parse_array(Buffer* buffer, std::vector<RespValue>& out) {
    const char* data = buffer->peek();
    size_t len = buffer->readable_bytes();

    // ----------------------------------------------------------------
    // 第1步：解析数组计数行
    // ----------------------------------------------------------------

    const char* crlf = find_crlf(data + 1, len - 1);
    if (!crlf) {
        error_msg_ = "Array missing count CRLF";
        return false;
    }

    std::string count_str(data + 1, crlf - (data + 1));
    int64_t count = std::stoll(count_str);

    // 跳过 *<count>\r\n
    skip(buffer, (crlf - data) + 2);

    // ----------------------------------------------------------------
    // 第2步：处理 null 数组
    // ----------------------------------------------------------------
    // *-1\r\n 表示 null 数组

    if (count < 0) {
        return true;
    }

    // ----------------------------------------------------------------
    // 第3步：解析每个元素
    // ----------------------------------------------------------------
    // 使用 reserve() 预分配内存，避免多次重新分配
    // reserve(n) 确保 vector 可以容纳 n 个元素而不重新分配

    out.reserve(static_cast<size_t>(count));

    for (int64_t i = 0; i < count; ++i) {
        // 检查是否还有数据
        if (buffer->readable_bytes() < 1) {
            error_msg_ = "Array element missing";
            return false;
        }

        // 递归调用 parse_one() 解析每个元素
        // 元素也可能是数组，所以需要递归

        RespValue elem;
        if (!parse_one(buffer, elem)) {
            return false;
        }

        out.push_back(elem);
    }

    return true;
}

// ============================================================================
// RespEncoder 编码器实现
// ============================================================================
// 编码器将数据转换为 RESP 格式的字符串
// 用于生成响应给客户端

// 编码简单字符串: +OK\r\n
std::string RespEncoder::encode_simple_string(const std::string& s) {
    // 字符串拼接: "+" + s + "\r\n"
    // C++ 中字符串可以用 + 运算符拼接
    return "+" + s + "\r\n";
}

// 编码错误: -ERR message\r\n
std::string RespEncoder::encode_error(const std::string& err) {
    // Redis 错误格式是 "-ERR <message>\r\n"
    // 所以错误消息会自动加上 "ERR " 前缀
    return "-ERR " + err + "\r\n";
}

// 编码整数 (int64_t 版本)
std::string RespEncoder::encode_integer(int64_t n) {
    // std::to_string() 将整数转换为字符串
    return ":" + std::to_string(n) + "\r\n";
}

// 编码整数 (int 版本，函数重载)
std::string RespEncoder::encode_integer(int n) {
    return ":" + std::to_string(n) + "\r\n";
}

// 编码批量字符串: $5\r\nhello\r\n
std::string RespEncoder::encode_bulk_string(const std::string& s) {
    // 格式: $<length>\r\n<content>\r\n
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

// 编码 null: $-1\r\n
std::string RespEncoder::encode_null() {
    return "$-1\r\n";
}

// 便捷方法: OK
std::string RespEncoder::encode_ok() {
    return "+OK\r\n";
}

// 便捷方法: nil
std::string RespEncoder::encode_nil() {
    return "$-1\r\n";
}

// 编码数组
std::string RespEncoder::encode_array(const std::vector<std::string>& arr) {
    // ----------------------------------------------------------------
    // std::ostringstream 用于高效拼接多个字符串
    // 类似于 std::string，但使用 << 运算符更高效
    // ----------------------------------------------------------------

    std::string result = "*" + std::to_string(arr.size()) + "\r\n";

    for (const auto& item : arr) {
        // for (const auto& item : arr) 是范围 for 循环
        // 遍历 arr 中的每个元素
        // const auto& 表示常量引用（不复制，不修改）

        result += encode_bulk_string(item);
    }

    return result;
}

}  // namespace cc_server
