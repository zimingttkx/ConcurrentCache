#include <iostream>
#include <vector>
#include <algorithm>
#include <functional>
#include "src/network/event_loop.h"
#include "src/network/socket.h"
#include "src/network/connection.h"
#include "src/command/command_factory.h"
#include "src/base/log.h"
#include "base/config.h"

using namespace cc_server;

// 命令处理回调函数
void handle_command(const RespValue& cmd, Connection* conn) {
    // 从 RespValue 提取命令数组
    const auto& arr = cmd.as_array();
    if (arr.empty()) {
        return;
    }

    // 提取命令参数
    std::vector<std::string> args;
    for (size_t i = 0; i < arr.size(); ++i) {
        args.push_back(arr[i].as_string());
    }

    // 命令名转小写（Redis 协议不区分大小写）
    if (!args.empty()) {
        std::string& cmd_name = args[0];
        std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    }

    // 创建并执行命令
    auto command = CommandFactory::instance().create(args[0]);
    if (command) {
        std::string response = command->execute(args);
        conn->send_response(response);
    } else {
        // 命令不存在
        conn->send_response(RespEncoder::encode_error("ERR unknown command"));
    }
}

int main() {
    // 初始化日志
    Logger::instance().setLevel(LogLevel::INFO);
    Logger::instance().setFile("logs/server.log");

    LOG_INFO(main, "=== ConcurrentCache Server Starting ===");

    if (!Config::instance().load("conf/concurrentcache.conf")) {
        LOG_ERROR(main, "Failed to load configuration");
        return 1;
    }

    Config::instance().addObserver("log_level", &Logger::instance());

    // 创建事件循环
    EventLoop loop;

    // 创建服务器监听套接字
    Socket server;
    if (!server.bind_and_listen(6379)) {
        LOG_ERROR(main, "Failed to start server on port 6379");
        return 1;
    }

    // 为服务器套接字创建 Channel，处理 accept 事件
    Channel server_channel(&loop, server.fd());
    server_channel.set_read_callback([&loop, &server]() {
        // 服务器套接字可读，说明有客户端连接请求
        int client_fd = server.accept();
        if (client_fd >= 0) {
            // 创建新的 Connection
            auto* conn = new Connection(client_fd, &loop);

            // 设置命令处理回调（使用函数指针）
            conn->set_command_callback(handle_command);
        }
    });
    server_channel.enable_reading();

    LOG_INFO(main, "Server ready, waiting for connections...");

    // 启动事件循环
    loop.loop();

    LOG_INFO(main, "=== ConcurrentCache Server Shutdown ===");
    return 0;
}
