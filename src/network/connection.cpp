#include "connection.h"

namespace cc_server {

    /**
     * =========================================================================
     * 构造函数：创建 Connection 对象，管理一个客户端连接
     * =========================================================================
     *
     * 参数：
     *   - client_fd: 已连接的客户端文件描述符（由 Server 的 accept() 获得）
     *   - loop: 指向所属 EventLoop 的指针，用于注册 IO 事件
     *
     * 初始化流程：
     *   1. client_socket_ 用 client_fd 构造，管理这个已连接套接字
     *   2. loop_ 保存指针，后续注册 Channel 需要用到
     *   3. channel_ = new Channel(loop, client_fd) 创建事件处理器
     *   4. 给 Channel 设置读/写/错误回调
     *   5. 启用读事件监听（EPOLLIN），等待客户端数据
     *
     * 为什么需要 input_buffer_ 和 output_buffer_？
     *   - TCP 是流协议，数据像"水流"一样过来，没有边界
     *   - input_buffer_ 缓存已收到但还没处理的数据
     *   - output_buffer_ 缓存要发送但还没发完的数据
     *   - 这样业务层可以按"消息"为单位处理，不关心 TCP 粘包
     */
    Connection::Connection(int client_fd, EventLoop* loop)
        : client_socket_(client_fd),   // 用 client_fd 构造 Socket，管理已连接套接字
          loop_(loop),                 // 保存 EventLoop 指针
          channel_(nullptr),           // 先设为 nullptr，后面 new
          input_buffer_(),            // 构造空输入缓冲区
          output_buffer_(),             // 构造空输出缓冲区
          resp_parser_()             // 构造 RESP 解析器
    {
        // Channel 负责监听这个客户端连接的 IO 事件
        // 当 epoll 检测到 client_fd 可读/可写时，通知 Connection 处理
        channel_ = new Channel(loop_, client_socket_.fd());

        // Channel 本身不处理业务，只是把事件分发给我们
        // 所以要告诉 Channel："事件来了调我这些函数"

        // 设置读回调：当 epoll 检测到 client_fd 可读（客户端发来数据）
        channel_->set_read_callback([this]() {
            this->handle_read();
        });

        // 设置写回调：当 epoll 检测到 client_fd 可写（可以继续发数据）
        channel_->set_write_callback([this]() {
            this->handle_write();
        });

        // 设置错误回调：当 epoll 检测到错误（连接断开等）
        channel_->set_error_callback([this]() {
            // 不要直接调用 close()，而是通知 SubReactor 来处理
            // 这样可以确保 Connection 正确地从 connections_ 中移除并销毁
            if (close_callback_) {
                close_callback_();
            }
        });

        // 启用读事件监听
        // 为什么要监听读？
        // - 服务器是被动的，客户端主动发数据过来
        // - 我们需要知道"什么时候有数据来了"
        // - EPOLLIN = 可读事件，数据来了或者对方关闭连接
        channel_->enable_reading();

        LOG_DEBUG(connection, "Connection created: fd=%d", client_socket_.fd());
    }

    /**
     * =========================================================================
     * 析构函数：清理 Connection 资源
     * =========================================================================
     *
     * 清理顺序很重要：
     *   1. 先从 epoll 移除 Channel（否则 epoll 还在监听已关闭的 fd）
     *   2. 再删除 Channel 对象
     *   3. Socket 析构会自动 close() 套接字
     *   4. Buffer 是自动管理的，不需要手动清理
     *
     * 为什么要先 remove_channel？
     *   - 如果不移除，epoll 还监听着一个已关闭的 fd
     *   - 这个 fd 可能被其他文件复用，导致 epoll 通知错误的事件
     */
    Connection::~Connection() {
        // 告诉 EventLoop："这个 Channel 我不要了，别再通知我了"
        if (channel_ != nullptr) {
            loop_->remove_channel(channel_);
        }

        delete channel_;
        channel_ = nullptr;

        LOG_DEBUG(connection, "Connection destroyed: fd=%d", client_socket_.fd());
        // 注意：client_socket_ 在这里析构，会自动 close(fd)
    }

    /**
     * =========================================================================
     * handle_read()：处理读事件（核心函数）
     * =========================================================================
     *
     * 什么时候被调用？
     *   - epoll 检测到 client_fd 可读（客户端发来数据）
     *   - EventLoop 调用 channel_->handle_event()
     *   - handle_event() 调用我们设置的 read_cb_，也就是这个函数
     *
     * handle_read 做了什么？
     *   1. 从 socket 读取数据到 input_buffer_
     *   2. 业务层从 input_buffer_ 取数据处理（这里模拟）
     *
     * 关于非阻塞 IO：
     *   - client_fd 在 accept() 时已设置 SOCK_NONBLOCK
     *   - recv() 如果没有数据，立即返回 -1，errno = EAGAIN/EWOULDBLOCK
     *   - 不会阻塞等待，epoll 已经告诉我们"有数据了"
     *
     * 关于 read 返回值：
     *   - > 0: 成功读到这么多字节
     *   - = 0: 对方关闭了连接（EOF）
     *   - -1: 出错了（EAGAIN 除外，那是正常的）
     */
    void Connection::handle_read() {
        // 如果连接已关闭，直接返回
        // 防止 use-after-free：当 close_callback_ 被触发后，Connection 会被销毁
        if (closed_) {
            return;
        }

        // 准备一个临时缓冲区
        // 为什么用临时缓冲区而不是直接往 input_buffer_ 写？
        // - input_buffer_ 有自己的管理方式（双指针）
        // - 临时 buffer 作为中转，更灵活
        char temp_buffer[4096];  // 4KB 临时缓冲区

        ssize_t bytes_read = client_socket_.recv(
            temp_buffer,                     // 接收缓冲区
            sizeof(temp_buffer) - 1          // 留一个位置给 '\0'（方便调试）
        );

        if (bytes_read > 0) {
            // ---- 成功读到数据 ----

            // 追加到输入缓冲区
            // input_buffer_ 现在缓存了所有收到的数据
            // 业务层可以从这里按"消息"为单位读取
            input_buffer_.append(temp_buffer, bytes_read);
            // 协议解析 调用RespParaser解析命令
            std::vector<RespValue> commands = resp_parser_.parse(input_buffer());
            for (auto & cmd : commands) {
                if (command_callback_) {
                    command_callback_(cmd, this);
                }
            }
            if (!resp_parser_.error().empty()) {
                LOG_ERROR(connection, "RESP parse error: %s", resp_parser_.error().c_str());
                send_response(RespEncoder::encode_error(resp_parser_.error()));
                resp_parser_.reset();  // 重置解析器状态，准备下一次解析
            }

            LOG_DEBUG(connection, "handle_read: read %zd bytes from fd=%d",
                      bytes_read, client_socket_.fd());

            // 业务处理（这里是示例，实际业务逻辑在别的地方）
            // 为什么这里不做业务处理？
            // - Connection 是网络层，不知道业务是什么
            // - 业务层应该注册一个"消息回调"来处理
            // - 但当前骨架版本简化了，我们先模拟一下
            //
            // 伪代码：
            // if (has_complete_message(input_buffer_)) {
            //     std::string msg = parse_message(input_buffer_);
            //     business_logic(msg);  // 处理业务
            // }

        } else if (bytes_read == 0) {
            // ---- 对方关闭了连接 ----
            // 这是正常的关闭流程，对方发送了 FIN
            LOG_INFO(connection, "Client closed connection: fd=%d", client_socket_.fd());
            close();  // 清理自己的资源

        } else {
            // ---- 读失败了 ----
            // EAGAIN/EWOULDBLOCK 是正常的：没有数据了，epoll 误报
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // 忽略，继续等下一次事件
            }
            // 其他错误：打印日志，关闭连接
            LOG_ERROR(connection, "recv() failed: fd=%d, error=%s",
                      client_socket_.fd(), strerror(errno));
            close();
        }
    }

    /**
     * =========================================================================
     * handle_write()：处理写事件（核心函数）
     * =========================================================================
     *
     * 什么时候被调用？
     *   - epoll 检测到 client_fd 可写（内核发送缓冲区有空位）
     *   - 一般是 output_buffer_ 有数据要发
     *
     * 为什么要可写才发？
     *   - TCP 发送是异步的
     *   - 内核维护一个发送缓冲区，如果满了 send() 会阻塞
     *   - 设置 SOCK_NONBLOCK 后，满了就返回 -1
     *   - epoll 告诉我们"可以发了"，我们再发
     *
     * handle_write 做了什么？
     *   1. 把 output_buffer_ 的数据发送到 socket
     *   2. 发完后禁用写事件（避免 busy loop）
     */
    void Connection::handle_write() {
        // 如果连接已关闭，直接返回
        // 防止 use-after-free：当 close_callback_ 被触发后，Connection 会被销毁
        if (closed_) {
            return;
        }

        if (output_buffer_.readable_bytes() == 0) {
            // 没有数据要发，禁用写事件
            // 为什么要禁用？
            // - epoll 默认是 level-triggered
            // - 如果不禁用，只要 socket 可写，epoll 就一直通知
            // - 导致 busy loop：epoll_wait 返回 → handle_write → 没数据 → epoll_wait 返回 ...
            channel_->disable_all();
            channel_->enable_reading();  // 只保留读事件
            return;
        }

        // 从 output_buffer_ 读数据，通过 socket 发送
        const char* data = output_buffer_.peek();        // 查看数据起始位置（不移动指针）
        size_t len = output_buffer_.readable_bytes();   // 获取可读字节数

        ssize_t bytes_written = client_socket_.send(data, len);

        if (bytes_written > 0) {
            // ---- 发送成功 ----

            // 移动读指针，标记这些数据已发送
            // 注意：不是删除数据，只是移动指针表示"这部分已发送"
            output_buffer_.retrieve(bytes_written);

            LOG_DEBUG(connection, "handle_write: wrote %zd bytes to fd=%d",
                      bytes_written, client_socket_.fd());

            // ---- 发完了？----
            // 如果 output_buffer_ 清空了，禁用写事件避免 busy loop
            if (output_buffer_.readable_bytes() == 0) {
                channel_->disable_all();
                channel_->enable_reading();  // 只保留读事件
            }

        } else if (bytes_written == -1) {
            // ---- 发送失败 ----
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核发送缓冲区满了，等下一次可写事件
                // 不要禁用写事件，epoll 会继续通知我们
                return;
            }
            // 其他错误
            LOG_ERROR(connection, "send() failed: fd=%d, error=%s",
                      client_socket_.fd(), strerror(errno));
            close();
        }
        // bytes_written == 0 的情况理论上不会发生
    }

    /**
     * =========================================================================
     * send_response()：业务层调用，发送响应数据（核心函数）
     * =========================================================================
     *
     * 什么时候被调用？
     *   - 业务层处理完请求，要给客户端回复
     *   - 业务层把响应数据写入 output_buffer_
     *   - 调用 send_response() 触发发送
     *
     * 为什么需要这个函数而不是直接 send()？
     *   - TCP 是流协议，一次 send() 不一定能把所有数据发完
     *   - 如果内核发送缓冲区满了，send() 可能只发了一部分
     *   - send_response() 把数据放入 output_buffer_，然后启用写事件
     *   - 等 epoll 通知"可以写了"，handle_write() 继续发送剩余数据
     *
     * 参数：
     *   - data: 要发送的数据指针
     *   - len: 要发送的数据长度
     */
    void Connection::send_response(const char* data, size_t len) {
        if (data == nullptr || len == 0) {
            return;  // 没什么好发的
        }

        if (client_socket_.fd() < 0) {
            return;  // 连接已关闭
        }

        output_buffer_.append(data, len);

        // 告诉 epoll："我想知道什么时候可以写"
        // 这样 handle_write() 会被调用，发完这些数据
        //
        // 为什么不直接发？
        // - 直接 send() 可能阻塞（如果缓冲区满）
        // - 虽然 socket 是 nonblock 的，send() 会返回 -1
        // - 但直接发没法处理"只发了一部分"的情况
        // - 用 output_buffer_ + 写事件 更可靠
        channel_->enable_writing();

        LOG_DEBUG(connection, "send_response: queued %zu bytes for fd=%d",
                  len, client_socket_.fd());
    }

    /**
     * =========================================================================
     * send_response() 重载版本：直接发送 string
     * =========================================================================
     */
    void Connection::send_response(const std::string& response) {
        send_response(response.data(), response.size());
    }

    /**
     * =========================================================================
     * close()：关闭连接（核心函数）
     * =========================================================================
     *
     * close() 做了以下事情：
     *   1. 设置 closed_ 标志，防止重复关闭
     *   2. 从 epoll 移除 Channel
     *   3. 关闭 socket
     *   4. 触发 close_callback_ 通知 SubReactor 销毁 Connection
     *
     * 为什么需要 closed_ 标志？
     *   - 防止在回调执行期间或之前重复调用 close()
     *   - handle_read/handle_write 检查此标志后可以提前返回
     */
    void Connection::close() {
        // 防止重复关闭
        if (closed_) {
            return;
        }
        closed_ = true;

        // 避免 epoll 还监听已关闭的 fd
        if (channel_ != nullptr) {
            loop_->remove_channel(channel_);
        }

        // client_socket_.close() 会：
        // - 关闭 fd
        // - 发送 FIN 给对方（如果是对方先关的，这步已经完成）
        // - 清理 socket 相关资源
        client_socket_.close();

        // 触发 close_callback_ 通知 SubReactor 来销毁 Connection
        // 注意：这会导致 Connection 被销毁，所以之后不应该再访问 this
        if (close_callback_) {
            close_callback_();
        }
    }

    /**
     * =========================================================================
     * 工具函数：获取输入缓冲区指针
     * =========================================================================
     *
     * 供业务层调用：
     *   - 业务层从 input_buffer_ 读取请求数据
     *   - 读取完后调用 retrieve() 标记数据已处理
     *
     * 为什么返回指针而不是引用？
     *   - 业务层可能需要判断是否为空指针
     *   - 也可以返回引用，但 nullptr 更明确
     */
    Buffer* Connection::input_buffer() {
        return &input_buffer_;
    }

    /**
     * =========================================================================
     * 工具函数：获取输出缓冲区指针
     * =========================================================================
     *
     * 供业务层调用：
     *   - 业务层把响应数据写入 output_buffer_
     *   - 然后调用 send_response() 触发发送
     */
    Buffer* Connection::output_buffer() {
        return &output_buffer_;
    }

    /**
     * =========================================================================
     * 工具函数：获取文件描述符
     * =========================================================================
     */
    int Connection::fd() const {
        return client_socket_.fd();
    }

}
