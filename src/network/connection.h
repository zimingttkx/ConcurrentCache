#ifndef CONCURRENTCACHE_NETWORK_CONNECTION_H
#define CONCURRENTCACHE_NETWORK_CONNECTION_H

#include "socket.h"
#include "base/log.h"
#include "event_loop.h"
#include "channel.h"
#include "buffer.h"
#include "protocol/resp.h"

namespace cc_server {
    /**
     * @brief Connection类：TCP 客户端连接封装
     *
     * 协作关系：
     * - 持有 Socket、Channel、Buffer，管理连接生命周期
     * - handle_read()/handle_write() 处理 IO 事件
     * - 通过 EventLoop 的 epoll 事件驱动
     * - 业务层从 input_buffer_ 读取请求，将响应写入 output_buffer_
     */
    class Connection {
    public:
        // 命令处理回调类型
        using CommandCallback = std::function<void(const RespValue& cmd, Connection* conn)>;

    private:
        Socket client_socket_;
        EventLoop* loop_;
        Channel* channel_;
        Buffer input_buffer_;
        Buffer output_buffer_;
        RespParser resp_parser_; // RESP 协议解析器
        CommandCallback command_callback_; // 命令处理回调函数

    public:
        Connection(int client_fd, EventLoop* loop);
        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator = (const Connection&) = delete;

        // IO 事件处理
        void handle_read();
        void handle_write();
        void send_response(const char* data, size_t len);
        void send_response(const std::string& response);
        void close();

        // 缓冲区访问
        Buffer* input_buffer();
        Buffer* output_buffer();

        void set_command_callback(CommandCallback cb) {
            command_callback_ = std::move(cb);
        }

        // 获取 fd
        [[nodiscard]] int fd() const;
    };
}

#endif