#ifndef CONCURRENTCACHE_NETWORK_BUFFER_H
#define CONCURRENTCACHE_NETWORK_BUFFER_H

#include <vector> // 动态数组 用于缓冲区底层连续存储
#include <string> // 用于数据转换
#include <algorithm> // 内部拷贝工具

namespace cc_server {
    /**
 * @brief Buffer类：专为TCP网络设计的动态读写缓冲区
 *
 * 核心设计思想：
 * 1. 「双指针」读写模型：用reader_idx/writer_idx标记读写位置，避免频繁拷贝数据
 * 2. 「自动扩容」：写入数据时自动扩展底层vector，无需手动管理内存
 * 3. 「空间压缩」：当已读数据占比过高时，自动将剩余数据前移，释放前端空闲空间
 * 4. 「连续内存」：底层用vector存储，保证数据是连续的，适配read()/write()系统调用
 *
 * 网络编程中的典型用途：
 * - 接收数据时：把recv()到的数据append到缓冲区，处理TCP粘包问题
 * - 发送数据时：直接从缓冲区读取数据发送，发送成功后retrieve已发送部分
 *
 * 注意：本类非线程安全，多线程使用时需额外加锁保护
 */
    class Buffer {
    private:
        std::vector<char> buffer_; // 底层存储 使用连续内存的动态数组
        size_t reader_idx_; // 读指针 标记下一个要读取的位置
        size_t writer_idx_; // 写指针 标记下一个要写入的位置
    public:
        /**
     * @brief 默认构造函数：创建空缓冲区，读写指针都指向开头
     *
     * 初始状态：底层vector为空，reader_idx_和writer_idx_都为0
     * 第一次append数据时，会自动触发扩容，无需手动初始化大小
     */
        Buffer() : reader_idx_(0), writer_idx_(0){}

        // ------------------- 核心状态查询 -------------------
        /**
         * @brief 获取当前缓冲区中「可读字节数」
         * @return 还未被读取的数据长度
         *
         * 原理：writer_idx_是下一个要写入的位置，reader_idx_是下一个要读取的位置
         * 两者的差值就是还没被读取的数据量，例如：
         * append 10字节后，writer_idx=10，reader_idx=0 → 可读10字节
         * retrieve 3字节后，reader_idx=3，writer_idx=10 → 可读7字节
         */
        size_t readable_bytes() const {
            return writer_idx_ - reader_idx_;
        }

            /**
         * @brief 获取当前缓冲区中「可写字节数」
         * @return 还能直接写入的空间大小（无需扩容）
         *
         * 原理：底层vector的总大小 - 写指针位置
         * 例如：vector大小为10，writer_idx=3 → 可写7字节
         * 若可写字节数不足，调用append时会自动扩容
         */
        size_t writable_bytes() const {
            return buffer_.size() - writer_idx_;
        }
        // ------------------- 写操作：向缓冲区追加数据 -------------------
        /**
         * @brief 向缓冲区追加数据（写入操作，自动扩容）
         * @param data 要写入的数据起始地址（支持任意内存块，比如recv()的缓冲区）
         * @param len 要写入的数据长度
         *
         * 典型场景：TCP recv()到数据后，直接append到缓冲区，处理粘包
         * 执行流程：
         * 1. 调用ensure_writable()确保有足够空间，不够则扩容
         * 2. 用std::copy把数据拷贝到写指针位置
         * 3. 移动写指针，标记数据写入完成
         */
        void append(const char* data, size_t len) {
            // 确保缓冲区有足够的空间
            ensure_writable(len);
            std::copy(data, data + len, buffer_.begin() + writer_idx_);
            writer_idx_ += len;
        }
        // ------------------- 读操作：查看/读取数据 -------------------
        /**
         * @brief 查看缓冲区中「可读数据的起始地址」（不移动读指针，无副作用）
         * @return 可读数据的首地址（连续内存，可直接传给系统调用）
         *
         * 关键特性：
         * - 不修改缓冲区状态，只是查看数据
         * - 地址是连续的，可直接用于readv/writev、memcpy等操作
         * 典型场景：处理TCP粘包时，先peek()看看有没有完整的数据包，再决定读多少
         */
        const char* peek() const {
            return buffer_.data() + reader_idx_;
        }

            /**
         * @brief 移动读指针，标记指定长度的数据为「已读」（不会删除数据，仅移动指针）
         * @param len 要标记为已读的字节数
         *
         * 关键设计：
         * - 不拷贝数据，仅移动reader_idx_，性能极高
         * - 当已读数据超过缓冲区一半时，自动触发compact()压缩空间
         * 典型场景：处理完一个完整的数据包后，调用retrieve()跳过这部分数据
         */
        void retrieve(size_t len) {
            if (len > readable_bytes()) {
                // 当读取长度超过当前可读数据 直接清空所有数据
                reader_idx_ = writer_idx_;
            }else {
                reader_idx_ += len;
            }

            if (reader_idx_ > buffer_.size() / 2) {
                compact();
            }
        }

            /**
         * @brief 清空缓冲区所有数据（读写指针归零）
         *
         * 典型场景：连接关闭后，重置缓冲区状态，准备下一次使用
         */
        void retrieve_all() {
            reader_idx_ = writer_idx_ = 0;
        }
        /**
         * @brief 把当前缓冲区中所有可读数据，转换为std::string
         * @return 包含所有可读数据的字符串（调用后缓冲区状态不变）
         *
         * 实现逻辑：peek()获取数据起始地址，readable_bytes()获取长度，构造string
         * 典型场景：处理文本协议（如HTTP、Redis）时，把数据转成字符串解析
         */
        std::string to_string() const {
            return std::string(peek(), readable_bytes());
        }
    private:
        /**
     * @brief 确保缓冲区有至少len字节的可写空间，不够则扩容
     * @param len 需要的可写字节数
     *
     * 扩容逻辑：直接把底层vector扩展到writer_idx_ + len的大小
     * 优化建议：工程中可改为「预分配扩容」，比如扩成max(writer_idx_ + len, buffer_.size() * 2)，减少扩容次数
     * （因为vector扩容会拷贝整个数据，频繁扩容会影响性能）
     */
        void ensure_writable(size_t len) {
            if (writable_bytes() < len) {
                buffer_.resize(writer_idx_ + len);
            }
        }
            /**
         * @brief 压缩缓冲区空间：把剩余的可读数据移到缓冲区开头，释放前端空闲空间
         *
         * 触发条件：reader_idx_ > buffer_.size() / 2（已读数据超过一半，空间浪费明显）
         * 执行流程：
         * 1. 把[reader_idx_, writer_idx_)区间的数据拷贝到缓冲区开头
         * 2. 更新writer_idx_和reader_idx_，读指针归零，写指针前移
         * 作用：避免reader_idx_一直后移，导致缓冲区前端大量空闲空间无法利用，防止内存泄漏
         */

        void compact() {
            if (reader_idx_ == 0) return;
            // 把可读数据拷贝到缓冲区开头
            std::copy(
                buffer_.begin() + reader_idx_, // 源：可读数据起始位置
                buffer_.begin() + writer_idx_, // 源：可读数据结束位置
                buffer_.begin()                // 目标：缓冲区开头
            );
            // 更新读写指针
            // 写指针减去已读长度 读指针归零
            writer_idx -= reader_idx_;
            reader_idx_ = 0;
        }
    };
}
#endif