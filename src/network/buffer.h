#ifndef CONCURRENTCACHE_NETWORK_BUFFER_H
#define CONCURRENTCACHE_NETWORK_BUFFER_H

#include <vector>
#include <string>

namespace cc_server {
    /**
     * @brief Buffer类：TCP 读写缓冲区
     *
     * 协作关系：
     * - 解决 TCP 粘包问题
     * - input_buffer_ 接收数据，output_buffer_ 发送数据
     * - 被 Connection 使用，管理收发数据
     *
     * 设计：双指针模型，reader_idx_/writer_idx_ 分离读写
     */
    class Buffer {
    private:
        std::vector<char> buffer_;
        size_t reader_idx_;
        size_t writer_idx_;

    public:
        Buffer();

        // 状态查询
        size_t readable_bytes() const;
        size_t writable_bytes() const;

        // 写操作
        void append(const char* data, size_t len);

        // 读操作
        const char* peek() const;
        void retrieve(size_t len);
        void retrieve_all();
        std::string to_string() const;

    private:
        void ensure_writable(size_t len);
        void compact();
    };
}

#endif