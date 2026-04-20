#include "buffer.h"
#include <algorithm>

namespace cc_server {
    // 构造函数：读写指针初始化为 0
    Buffer::Buffer() : reader_idx_(0), writer_idx_(0) {}

    // 可读字节数 = writer_idx_ - reader_idx_
    size_t Buffer::readable_bytes() const {
        return writer_idx_ - reader_idx_;
    }

    // 可写字节数 = vector大小 - writer_idx_
    size_t Buffer::writable_bytes() const {
        return buffer_.size() - writer_idx_;
    }

    // 追加数据
    // - ensure_writable() 确保空间足够（自动扩容）
    // - std::copy 拷贝数据到写指针位置
    // - 移动写指针
    void Buffer::append(const char* data, size_t len) {
        ensure_writable(len);
        std::copy(data, data + len, buffer_.begin() + writer_idx_);
        writer_idx_ += len;
    }

    // 查看可读数据起始地址（不移动指针）
    const char* Buffer::peek() const {
        return buffer_.data() + reader_idx_;
    }

    // 移动读指针，标记数据为已读
    // - 超过可读数据量时直接归零
    // - reader_idx_ 超过一半时触发 compact() 压缩空间
    void Buffer::retrieve(size_t len) {
        if (len > readable_bytes()) {
            reader_idx_ = writer_idx_;
        } else {
            reader_idx_ += len;
        }

        if (reader_idx_ > buffer_.size() / 2) {
            compact();
        }
    }

    // 清空缓冲区：读写指针归零
    void Buffer::retrieve_all() {
        reader_idx_ = writer_idx_ = 0;
    }

    // 转换为 string
    std::string Buffer::to_string() const {
        return std::string(peek(), readable_bytes());
    }

    // 确保有足够可写空间，不够则扩容
    void Buffer::ensure_writable(size_t len) {
        if (writable_bytes() < len) {
            buffer_.resize(writer_idx_ + len);
        }
    }

    // 压缩空间：把未读数据移到缓冲区开头，释放前端空闲
    void Buffer::compact() {
        if (reader_idx_ == 0) return;
        std::copy(
            buffer_.begin() + reader_idx_,
            buffer_.begin() + writer_idx_,
            buffer_.begin()
        );
        writer_idx_ -= reader_idx_;
        reader_idx_ = 0;
    }
}