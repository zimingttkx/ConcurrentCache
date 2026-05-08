//
// RDB 持久化头文件
//

#ifndef CONCURRENTCACHE_RDB_H
#define CONCURRENTCACHE_RDB_H

#include <string>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <ctime>

namespace cc_server {
    class GlobalStorage;
    class CacheObject;

    // RDB 文件魔数 "CCRD"
    constexpr uint32_t kRdbMagic = 0x43435244;
    // RDB 版本
    constexpr uint8_t kRdbVersion[4] = {'0', '0', '0', '2'};

    // KV pair 类型
    enum class RdbValueType : uint8_t {
        STRING = 0,
        LIST = 1,
        HASH = 2,
        SET = 3,
        ZSET = 4
    };

    // RDB 特殊标记
    enum class RdbSpecialMarker : uint8_t {
        KV_WITH_TTL = 0xFE,  // 带 TTL 的 KV pair
        EOF_MARKER = 0xFF    // 文件结束标记
    };

    // BGSAVE 状态
    enum class BgsaveStatus {
        IDLE = 0,           // 空闲
        IN_PROGRESS = 1,    // 正在进行
        SUCCESS = 2,        // 上次成功
        FAILED = 3          // 上次失败
    };

    // RDB 统计信息
    struct RdbStats {
        std::atomic<int64_t> last_bgsave_time_sec{0};      // 上次 BGSAVE 时间戳
        std::atomic<BgsaveStatus> last_bgsave_status{BgsaveStatus::IDLE};  // 上次状态
        std::atomic<size_t> last_bgsave_keys{0};           // 上次保存的 key 数量
        std::atomic<size_t> total_bgsave_calls{0};         // BGSAVE 调用次数
        std::atomic<size_t> total_rdb_saved_keys{0};       // 总共保存的 key 数量
    };

    class RdbPersistence {
    public:
        RdbPersistence();
        ~RdbPersistence();

        // 禁用拷贝
        RdbPersistence(const RdbPersistence&) = delete;
        RdbPersistence& operator=(const RdbPersistence&) = delete;

        /**
         * @brief 获取单例实例
         */
        static RdbPersistence& instance() {
            static RdbPersistence instance;
            return instance;
        }

        /**
         * @brief 同步保存 RDB 文件
         */
        bool save(const std::string& filepath, GlobalStorage& storage);

        /**
         * @brief 异步保存 RDB 文件（fork 子进程执行）
         */
        bool save_in_background(const std::string& filepath, GlobalStorage& storage);

        /**
         * @brief 加载 RDB 文件
         */
        bool load(const std::string& filepath, GlobalStorage& storage);

        /**
         * @brief 获取 BGSAVE 是否正在进行
         */
        bool is_bgsave_in_progress() const {
            return bgsave_in_progress_.load(std::memory_order_acquire) == 1;
        }

        /**
         * @brief 获取上次 BGSAVE 状态
         */
        BgsaveStatus get_last_bgsave_status() const {
            return stats_.last_bgsave_status.load(std::memory_order_acquire);
        }

        /**
         * @brief 获取上次 BGSAVE 时间戳
         */
        int64_t get_last_bgsave_time() const {
            return stats_.last_bgsave_time_sec.load(std::memory_order_acquire);
        }

        /**
         * @brief 获取上次保存的 key 数量
         */
        size_t get_last_bgsave_keys() const {
            return stats_.last_bgsave_keys.load(std::memory_order_acquire);
        }

        /**
         * @brief 获取 RDB 统计信息
         */
        const RdbStats& get_stats() const { return stats_; }

        /**
         * @brief 设置保存路径
         */
        void set_filepath(const std::string& filepath) { filepath_ = filepath; }

        /**
         * @brief 获取保存路径
         */
        const std::string& get_filepath() const { return filepath_; }

        /**
         * @brief 等待后台保存完成（同步等待子进程）
         * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
         * @return true=保存成功，false=超时或失败
         */
        bool wait_for_bgsave(int timeout_ms = 5000);

    private:
        // 写入辅助方法
        void write_uint32(uint32_t val);
        void write_uint64(uint64_t val);
        void write_uint8(uint8_t val);
        void write_string(const std::string& val);

        // 读取辅助方法
        uint32_t read_uint32();
        uint64_t read_uint64();
        uint8_t read_uint8();
        std::string read_string();

        // 写入 KV pair（带 TTL）
        void write_kv_pair(const std::string& key, const CacheObject& value, int64_t expire_time_ms);

        // 读取 KV pair
        bool read_kv_pair(std::string& key, CacheObject& value, int64_t& expire_time_ms);

        // 序列化方法
        void serialize_string(const std::string& val);
        void serialize_list(const CacheObject& obj);
        void serialize_hash(const CacheObject& obj);
        void serialize_set(const CacheObject& obj);
        void serialize_zset(const CacheObject& obj);

        // 反序列化方法
        bool deserialize_string(CacheObject& obj);
        bool deserialize_list(CacheObject& obj);
        bool deserialize_hash(CacheObject& obj);
        bool deserialize_set(CacheObject& obj);
        bool deserialize_zset(CacheObject& obj);

        // CRC32 计算（指定结束位置，排除 CRC 本身）
        uint32_t calculate_crc32_for_range(long end_pos);

        // 更新 BGSAVE 状态
        void update_bgsave_status(BgsaveStatus status, size_t keys);

        // 获取当前时间戳（秒）
        int64_t current_time_sec() const {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        FILE* file_ = nullptr;
        std::string filepath_;
        std::atomic<int> bgsave_in_progress_{0};  // 原子标记是否正在保存
        RdbStats stats_;
    };

}  // namespace cc_server

#endif  // CONCURRENTCACHE_RDB_H
