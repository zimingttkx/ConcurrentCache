//
// sub_reactor.cpp
// SubReactor实现
//

#include "sub_reactor.h"
#include "command/command_factory.h"
#include "protocol/resp.h"
#include <algorithm>
#include <cctype>

namespace cc_server {

std::unique_ptr<SubReactor> SubReactor::create() {
    return std::unique_ptr<SubReactor>(new SubReactor());
}

SubReactor::SubReactor()
    : loop_(std::make_unique<EventLoop>())
    , thread_(nullptr)
    , connection_count_(0) {
}

SubReactor::~SubReactor() {
    stop();
}

void SubReactor::start() {
    if (thread_ != nullptr) {
        return;  // 已经启动
    }

    // 创建独立线程运行事件循环
    thread_ = new std::thread([this]() {
        loop_->loop();
    });
}

void SubReactor::stop() {
    // 退出事件循环
    if (loop_) {
        loop_->quit();
    }

    // 等待线程结束
    if (thread_ != nullptr) {
        if (thread_->joinable()) {
            thread_->join();
        }
        delete thread_;
        thread_ = nullptr;
    }
}

void SubReactor::stop_without_join() {
    // 只 quit 事件循环，不 join
    if (loop_) {
        loop_->quit();
    }
}

void SubReactor::join_thread() {
    std::cout << "[SubReactor] join_thread() 被调用" << std::endl;
    if (thread_ != nullptr) {
        std::cout << "[SubReactor] thread_ 不为 nullptr，检查 joinable" << std::endl;
        if (thread_->joinable()) {
            std::cout << "[SubReactor] 线程可 join，开始等待..." << std::endl;
            thread_->join();
            std::cout << "[SubReactor] 线程已 join 完成" << std::endl;
        } else {
            std::cout << "[SubReactor] 线程不可 join" << std::endl;
        }
        delete thread_;
        thread_ = nullptr;
    } else {
        std::cout << "[SubReactor] thread_ 是 nullptr" << std::endl;
    }
}

    void SubReactor::add_connection(int client_fd) {
    auto conn = std::make_unique<Connection>(client_fd, loop_.get());

    conn->set_read_callback([conn_ptr = conn.get()]() {
        handle_read(conn_ptr);
    });

    conn->set_write_callback([conn_ptr = conn.get()]() {
        handle_write(conn_ptr);
    });

    conn->set_close_callback([this, conn_ptr = conn.get()]() {
        handle_close(conn_ptr);
    });

    // 步骤3：设置命令处理回调
    conn->set_command_callback([](const RespValue& cmd, Connection* conn) {
        const auto& arr = cmd.as_array();
        if (arr.empty()) return;

        // 构建完整的参数列表（包含命令名）
        std::vector<std::string> args;
        for (size_t i = 0; i < arr.size(); ++i) {
            args.push_back(arr[i].as_string());
        }

        // 将命令名转换为小写（Redis 命令不区分大小写）
        std::string cmd_name = args[0];
        std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        auto command = CommandFactory::instance().create(cmd_name);
        if (command) {
            auto response = command->execute(args);
            conn->send_response(response);
        } else {
            // 命令不存在，返回错误响应
            conn->send_response(RespEncoder::encode_error("unknown command '" + args[0] + "'"));
        }
    });

    conn->channel()->enable_reading();
    loop_->update_channel(conn->channel());

    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        connections_[client_fd] = std::move(conn);
    }

    connection_count_.fetch_add(1, std::memory_order_relaxed);
}


void SubReactor::handle_read(Connection* conn) {
    // 读取客户端数据
    // 注意：这个函数是在SubReactor的EventLoop线程中调用的
    // 所以对conn的访问不需要加锁
    conn->handle_read();
}

void SubReactor::handle_write(Connection* conn) {
    conn->handle_write();
}

void SubReactor::handle_close(Connection* conn) {
    // 客户端主动关闭连接，或者发生错误
    LOG_INFO(NETWORK, "SubReactor connection closed, fd=%d", conn->fd());
    remove_connection(conn);
}

void SubReactor::remove_connection(Connection* conn) {
    int fd = conn->fd();

    // 从EventLoop移除Channel
    loop_->remove_channel(conn->channel());

    // 从connections_中移除
    {
        std::unique_lock<std::shared_mutex> lock(connections_mutex_);
        connections_.erase(fd);
    }

    connection_count_.fetch_sub(1, std::memory_order_relaxed);

    // Connection的析构函数会关闭fd
}

} // namespace cc_server
