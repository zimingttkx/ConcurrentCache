//
// RDB 持久化实现
//

#include "persistence/rdb.h"
#include "cache/storage.h"
#include "base/log.h"
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
#include <zlib.h>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <process.h>
    typedef HANDLE ProcessHandle;
#else
    #include <unistd.h>
    #include <signal.h>
    #include <sys/wait.h>
    typedef pid_t ProcessHandle;
#endif

// 跨平台字节序转换
#if defined(_WIN32)
#include <winsock2.h>
#elif defined(__APPLE__)
#include <machine/endian.h>
#else
#include <endian.h>
#endif

namespace cc_server {

// 日志模块名
static constexpr const char* kRdbModule = "RDB";

// 获取当前可执行文件路径（用于跨平台子进程创建）
[[maybe_unused]] static std::string get_executable_path() {
#if defined(_WIN32)
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (len == 0 || len >= MAX_PATH) {
        return "concurrentcache-server";
    }
    // 去掉引号
    std::string path(exe_path);
    if (!path.empty() && path.front() == '"') {
        path = path.substr(1);
    }
    if (!path.empty() && path.back() == '"') {
        path.pop_back();
    }
    return path;
#else
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        return "./concurrentcache-server";
    }
    exe_path[len] = '\0';
    return exe_path;
#endif
}

// 字节序转换辅助
static inline uint32_t host_to_big32(uint32_t val) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
#elif defined(_WIN32)
    return _byteswap_ulong(val);
#elif defined(htobe32)
    return htobe32(val);
#else
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
#endif
}

static inline uint64_t host_to_big64(uint64_t val) {
#if defined(_WIN32)
    return _byteswap_uint64(val);
#else
    return htobe64(val);
#endif
}

static inline uint32_t big_to_host32(uint32_t val) {
    return host_to_big32(val);  // 对称的
}

static inline uint64_t big_to_host64(uint64_t val) {
    return host_to_big64(val);  // 对称的
}

// RAII 文件句柄封装
struct FileCloser {
    void operator()(FILE* fp) const {
        if (fp) fclose(fp);
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

RdbPersistence::RdbPersistence() = default;

RdbPersistence::~RdbPersistence() {
    // 进程退出时等后台快照线程结束。以前它是 detach() 的：主线程已经开始析构
    // GlobalStorage / Logger 这些函数局部静态对象，快照线程那边还在遍历 storage
    // 的快照并往文件里写 —— 退出时的 use-after-free。RdbPersistence 比 GlobalStorage
    // 晚构造，所以它的析构先发生，这里 join 是安全的顺序。
    std::lock_guard<std::mutex> guard(snapshot_mutex_);
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
}

bool RdbPersistence::save(const std::string& filepath, GlobalStorage& storage) {
    // 防止与后台 save 或其他 save() 调用并发写同一文件
    std::lock_guard<std::mutex> save_lock(save_mutex_);

    // 原子写：先写临时文件，完成后 fsync + rename 覆盖目标，避免崩溃留下残缺文件
    std::string tmp_path = filepath + ".tmp";
    uint32_t saved_keys = 0;
    {
        FilePtr file_guard(fopen(tmp_path.c_str(), "w+b"));
        file_ = file_guard.get();
        if (!file_) {
            LOG_ERROR(RDB, "Failed to open temp file for save: %s", tmp_path.c_str());
            return false;
        }
        filepath_ = tmp_path;

        try {
            // 1. 写入 Header
            // MAGIC: "CCRD"
            write_uint32(kRdbMagic);

            // VERSION: "0002"
            fwrite(kRdbVersion, 4, 1, file_);

            // 2. 写入数据库数量（暂时只支持 DB 0）
            uint32_t db_count = 1;
            write_uint32(db_count);

            // 3. 获取所有 KV pairs（包括 TTL 信息）
            auto all_kvs = storage.get_all_objects_with_ttl();

            // 写入 KV pairs 数量
            uint32_t kv_count = static_cast<uint32_t>(all_kvs.size());
            write_uint32(kv_count);

            LOG_INFO(RDB, "Saving %zu key-value pairs to %s", all_kvs.size(), filepath.c_str());

            // 4. 写入每个 KV pair（带 TTL）
            for (const auto& [key, obj, expire_time_ms] : all_kvs) {
                write_kv_pair(key, obj, expire_time_ms);
            }

            // 刷新缓冲区，确保所有数据都被写入文件
            fflush(file_);

            // 5. 写入 EOF 标记
            write_uint8(static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER));

            // 6. 记录 CRC 位置（此时游标在 EOF 之后）
            long crc_pos = ftell(file_);

            // 7. 计算 CRC（从文件开头到当前位置）
            uint32_t crc = calculate_crc32_for_range(crc_pos);

            // 8. 写入 CRC 值（使用 write_uint32 保证字节序转换）
            write_uint32(crc);
            fflush(file_);

            // 强制刷盘，避免缓存丢失
            if (fflush(file_) != 0 || fsync(fileno(file_)) != 0) {
                throw std::runtime_error("fsync failed during save");
            }

            // file_guard 会自动关闭文件
            file_ = nullptr;
            saved_keys = kv_count;

        } catch (const std::exception& e) {
            LOG_ERROR(RDB, "Exception during save: %s", e.what());
            record_save_result(BgsaveStatus::FAILED, 0);
            // file_guard 会自动关闭文件
            file_ = nullptr;
            // 清理可能残留的临时文件
            std::remove(tmp_path.c_str());
            return false;
        }
    }

    // 原子替换：rename 是原子操作，要么看到旧文件要么看到新文件
    if (rename(tmp_path.c_str(), filepath.c_str()) != 0) {
        LOG_ERROR(RDB, "Failed to rename temp file %s to %s: %s",
                  tmp_path.c_str(), filepath.c_str(), strerror(errno));
        std::remove(tmp_path.c_str());
        // rename 失败就是这次快照没落盘。以前统计信息在 rename 之前就已经记成
        // SUCCESS 了，INFO 里的 lastsave 时间会指向一个根本没替换成功的保存。
        record_save_result(BgsaveStatus::FAILED, 0);
        return false;
    }

    // 同步目录元数据（确保 rename 持久化）
    std::string dir = filepath.substr(0, filepath.find_last_of('/'));
    if (!dir.empty()) {
        int dirfd = open(dir.c_str(), O_RDONLY);
        if (dirfd >= 0) {
            fsync(dirfd);
            close(dirfd);
        }
    }

    record_save_result(BgsaveStatus::SUCCESS, saved_keys);

    LOG_INFO(RDB, "RDB save completed: %s, keys=%u", filepath.c_str(),
             static_cast<uint32_t>(storage.size()));
    return true;
}

bool RdbPersistence::save_in_background(const std::string& filepath, GlobalStorage& storage) {
    // 检查是否已经有 BGSAVE 在运行
    int expected = 0;
    if (!bgsave_in_progress_.compare_exchange_strong(expected, 1)) {
        LOG_WARN(RDB, "BGSAVE already in progress, skipping");
        return false;
    }

#if defined(_WIN32)
// Windows 命令行参数转义函数
static std::string escape_windows_arg(const std::string& arg) {
    std::string result;
    result.reserve(arg.size() + 10);

    for (size_t i = 0; i < arg.size(); ++i) {
        char c = arg[i];

        // 转义引号
        if (c == '"') {
            result += "\\\"";
        }
        // 转义反斜杠（如果后面跟着引号或在字符串末尾）
        else if (c == '\\') {
            size_t num_backslashes = 1;
            while (i + 1 < arg.size() && arg[i + 1] == '\\') {
                ++i;
                ++num_backslashes;
            }

            // 如果后面是引号或字符串末尾，需要双倍反斜杠
            if (i + 1 >= arg.size() || arg[i + 1] == '"') {
                result.append(num_backslashes * 2, '\\');
            } else {
                result.append(num_backslashes, '\\');
            }
        }
        else {
            result += c;
        }
    }

    return result;
}

// Windows: 使用 CreateProcess 创建子进程
    // 构建命令行（转义文件路径防止命令注入）
    std::string exe_path = get_executable_path();
    std::string escaped_filepath = escape_windows_arg(filepath);
    std::string cmd_line = exe_path + " --rdb-save \"" + escaped_filepath + "\"";

    // 设置进程属性
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!created) {
        LOG_ERROR(RDB, "CreateProcess() failed for background save, error=%lu", GetLastError());
        bgsave_in_progress_.store(0);
        return false;
    }

    // 关闭子进程的句柄，避免资源泄漏
    CloseHandle(pi.hThread);

    // 保存进程句柄供后续等待
    ProcessHandle process_handle = pi.hProcess;

    LOG_INFO(RDB, "Background save started, pid=%lu", GetProcessId(pi.hProcess));

    // 启动后台线程等待子进程
    std::thread wait_thread([this, process_handle]() {
        // 等待进程结束
        WaitForSingleObject(process_handle, INFINITE);

        // 获取退出码
        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);
        CloseHandle(process_handle);

        bool success = (exit_code == 0);
        stats_.last_bgsave_status.store(
            success ? BgsaveStatus::SUCCESS : BgsaveStatus::FAILED,
            std::memory_order_release);
        LOG_INFO(RDB, "Background save completed, exit_code=%lu, success=%d", exit_code, success);

        bgsave_in_progress_.store(0, std::memory_order_release);
    });
    wait_thread.detach();

#else
    // 方案 A：不使用 fork，直接在当前进程的独立线程内快照写临时文件
    // 规避 fork-in-multithreaded 下自定义内存池锁的死锁/UB 风险
    //
    // 初始状态必须在起线程之前落好：放到后面的话，一次快完成的快照会先把状态写成
    // SUCCESS，然后这行又把它盖回 IN_PROGRESS，INFO 就永远停在"还在跑"。
    // last_bgsave_time_sec 不在这里写 —— 它表示"最后一次成功保存的时间"，
    // 由 save() 成功收尾时的 record_save_result 负责。
    stats_.last_bgsave_status.store(BgsaveStatus::IN_PROGRESS, std::memory_order_release);
    stats_.total_bgsave_calls.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> guard(snapshot_mutex_);
        // 能走到这里说明 bgsave_in_progress_ 刚被我们抢下来，上一轮快照线程已经
        // 跑完最后一条语句（它就是以清零收尾的），join 只是回收线程对象。
        if (snapshot_thread_.joinable()) {
            snapshot_thread_.join();
        }
        snapshot_thread_ = std::thread([this, filepath, &storage]() {
            bool success = save(filepath, storage);
            stats_.last_bgsave_status.store(
                success ? BgsaveStatus::SUCCESS : BgsaveStatus::FAILED,
                std::memory_order_release);
            LOG_INFO(RDB, "Background save completed (in-process thread), success=%d", success);
            bgsave_in_progress_.store(0, std::memory_order_release);
        });
    }

    LOG_INFO(RDB, "Background save started (in-process thread)");

    return true;
}

#endif

bool RdbPersistence::save_to_temp_and_rename(const std::string& filepath, GlobalStorage& storage) {
    // 原子写实现：写临时文件后 rename（详见 save()）
    return save(filepath, storage);
}

bool RdbPersistence::wait_for_bgsave(int timeout_ms) {
    if (!is_bgsave_in_progress()) {
        return true;
    }

    // 使用简单的轮询等待
    auto start = std::chrono::steady_clock::now();
    while (is_bgsave_in_progress()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (timeout_ms >= 0 && elapsed > timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool RdbPersistence::load(const std::string& filepath, GlobalStorage& storage) {
    FilePtr file_guard(fopen(filepath.c_str(), "rb"));
    file_ = file_guard.get();
    if (!file_) {
        LOG_INFO(RDB, "RDB file not found: %s, starting fresh", filepath.c_str());
        return false;
    }
    filepath_ = filepath;

    // 使用 RAII 自动管理文件句柄（file_guard 已在上面创建）

    // 0 字节的 RDB 不是损坏，是"还没写过任何数据"。Redis 同样把空文件当空数据集
    // 接受，测试脚本也普遍用 touch 造一个空 dump.rdb 表示"从空存储启动"。
    // 若让它走到 magic 读取失败，一个合法的首次启动就变成"文件损坏、拒绝启动"。
    fseek(file_, 0, SEEK_END);
    const long initial_size = ftell(file_);
    fseek(file_, 0, SEEK_SET);
    if (initial_size == 0) {
        LOG_INFO(RDB, "RDB file is empty: %s, starting with an empty dataset", filepath.c_str());
        file_ = nullptr;
        return true;
    }

    try {
        // 1. 读取并验证 Header
        uint32_t magic = read_uint32();
        if (magic != kRdbMagic) {
            LOG_ERROR(RDB, "Invalid RDB magic: 0x%08X", magic);
            // file_guard 会自动关闭文件
            file_ = nullptr;
            return false;
        }

        // 读取版本
        char version[5] = {0};
        if (fread(version, 4, 1, file_) != 1) {
            LOG_ERROR(RDB, "Failed to read RDB version");
            // file_guard 会自动关闭文件
            file_ = nullptr;
            return false;
        }
        LOG_INFO(RDB, "RDB version: %.4s", version);

        // 修复 P1-11：校验版本，不允许以不兼容格式加载（同 magic 不同 layout 会损坏数据）
        if (std::memcmp(version, kRdbVersion, 4) != 0) {
            LOG_ERROR(RDB, "Unsupported RDB version: %.4s (expected %.4s)", version, kRdbVersion);
            file_ = nullptr;
            return false;
        }

        // 2. 先验 CRC，再动存储。
        //
        // 原来的顺序是：把整个文件逐条写进 storage，读到末尾才算 CRC。于是一个被
        // 截断或改坏的文件会先把半个数据集留在内存里，然后 load() 返回 false，
        // 而调用方把这当成"没有 RDB 文件"，服务器带着这批来路不全的数据对外服务。
        // CRC 覆盖 [0, filesize-4)，最后 4 字节就是它自己，所以可以在读正文之前
        // 先验完。
        const long body_pos = ftell(file_);
        fseek(file_, 0, SEEK_END);
        const long file_size = ftell(file_);
        if (file_size < 4) {
            LOG_ERROR(RDB, "RDB file too short to contain a CRC trailer: %ld bytes", file_size);
            file_ = nullptr;
            return false;
        }
        const long data_len = file_size - 4;
        fseek(file_, data_len, SEEK_SET);
        const uint32_t stored_crc = read_uint32();
        const uint32_t calculated_crc = calculate_crc32_for_range(data_len);
        if (stored_crc != calculated_crc) {
            LOG_ERROR(RDB, "CRC32 mismatch, refusing to load: stored=0x%08X, calculated=0x%08X",
                      stored_crc, calculated_crc);
            file_ = nullptr;
            return false;
        }
        fseek(file_, body_pos, SEEK_SET);

        // 3. 读取数据库数量
        uint32_t db_count = read_uint32();
        LOG_INFO(RDB, "RDB contains %u databases", db_count);

        // 3. 读取 KV pairs
        for (uint32_t db = 0; db < db_count; ++db) {
            uint32_t kv_count = read_uint32();
            LOG_INFO(RDB, "Loading %u key-value pairs from DB %u", kv_count, db);

            for (uint32_t i = 0; i < kv_count; ++i) {
                std::string key;
                CacheObject obj;
                int64_t expire_time_ms = -1;  // -1 表示永不过期

                if (read_kv_pair(key, obj, expire_time_ms)) {
                    // 如果有过期时间，使用 set_with_expire 原子性设置
                    if (expire_time_ms > 0) {
                        int64_t ttl_ms = expire_time_ms - storage.current_time_ms();
                        if (ttl_ms > 0) {
                            storage.set_with_expire(key, obj, ttl_ms);
                        } else {
                            // 已过期，不加载
                            LOG_DEBUG(RDB, "Skip expired key: %s", key.c_str());
                        }
                    } else {
                        // 永不过期
                        storage.set(key, obj);
                    }
                }
            }
        }

        // 4. 读取 EOF 标记
        uint8_t eof = read_uint8();
        if (eof != static_cast<uint8_t>(RdbSpecialMarker::EOF_MARKER)) {
            LOG_WARN(RDB, "Unexpected EOF marker: 0x%02X", eof);
        }

        // CRC 已经在第 2 步验过（那时一个字节都还没写进存储），这里不必再读一遍
        // 尾部 4 字节；文件合法性以那次校验为准。

        // file_guard 会自动关闭文件
        file_ = nullptr;

        LOG_INFO(RDB, "RDB load completed successfully");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(RDB, "Exception during load: %s", e.what());
        // file_guard 会自动关闭文件
        file_ = nullptr;
        return false;
    }
}

void RdbPersistence::record_save_result(BgsaveStatus status, size_t keys) {
    if (status == BgsaveStatus::SUCCESS) {
        // 只记统计。bgsave_in_progress_ 由启动快照的那个线程负责清零（见 rdb.h 的
        // 注释）：以前这里也顺手把它 store(0)，客户端在 BGSAVE 跑的时候敲一条 SAVE
        // 就会让服务器以为后台快照已经结束，紧接着第二条 BGSAVE 也能启动。
        stats_.last_bgsave_time_sec.store(current_time_sec(), std::memory_order_release);
        stats_.last_bgsave_keys.store(keys, std::memory_order_release);
        stats_.total_rdb_saved_keys.fetch_add(keys, std::memory_order_relaxed);
    }
    // FAILED 不动 lastsave / keys：一次失败的保存没有"最后成功保存时间"可更新。

    stats_.last_bgsave_status.store(status, std::memory_order_release);
}

// ============ 私有辅助方法 ============

void RdbPersistence::write_uint32(uint32_t val) {
    uint32_t be = host_to_big32(val);
    if (fwrite(&be, sizeof(be), 1, file_) != 1) {
        LOG_ERROR(RDB, "write_uint32 - fwrite failed");
        throw std::runtime_error("fwrite failed in write_uint32");
    }
}

void RdbPersistence::write_uint64(uint64_t val) {
    uint64_t be = host_to_big64(val);
    if (fwrite(&be, sizeof(be), 1, file_) != 1) {
        LOG_ERROR(RDB, "write_uint64 - fwrite failed");
        throw std::runtime_error("fwrite failed in write_uint64");
    }
}

void RdbPersistence::write_uint8(uint8_t val) {
    if (fwrite(&val, sizeof(val), 1, file_) != 1) {
        LOG_ERROR(RDB, "write_uint8 - fwrite failed");
        throw std::runtime_error("fwrite failed in write_uint8");
    }
}

void RdbPersistence::write_string(const std::string& val) {
    write_uint32(static_cast<uint32_t>(val.size()));
    if (!val.empty()) {
        if (fwrite(val.data(), 1, val.size(), file_) != val.size()) {
            LOG_ERROR(RDB, "write_string - fwrite failed");
            throw std::runtime_error("fwrite failed in write_string");
        }
    }
}

uint32_t RdbPersistence::read_uint32() {
    uint32_t be;
    if (fread(&be, sizeof(be), 1, file_) != 1) {
        LOG_ERROR(RDB, "read_uint32 - fread failed");
        throw std::runtime_error("fread failed in read_uint32");
    }
    return big_to_host32(be);
}

uint64_t RdbPersistence::read_uint64() {
    uint64_t be;
    if (fread(&be, sizeof(be), 1, file_) != 1) {
        LOG_ERROR(RDB, "read_uint64 - fread failed");
        throw std::runtime_error("fread failed in read_uint64");
    }
    return big_to_host64(be);
}

uint8_t RdbPersistence::read_uint8() {
    uint8_t val;
    if (fread(&val, sizeof(val), 1, file_) != 1) {
        LOG_ERROR(RDB, "read_uint8 - fread failed");
        throw std::runtime_error("fread failed in read_uint8");
    }
    return val;
}

std::string RdbPersistence::read_string() {
    uint32_t len = read_uint32();
    if (len == 0) return "";
    std::string val(len, '\0');
    if (fread(val.data(), 1, len, file_) != len) {
        LOG_ERROR(RDB, "read_string - fread failed, expected %u bytes", len);
        throw std::runtime_error("fread failed in read_string");
    }
    return val;
}

void RdbPersistence::write_kv_pair(const std::string& key, const CacheObject& value, int64_t expire_time_ms) {
    // 检查是否有过期时间
    bool has_ttl = (expire_time_ms > 0);

    // 如果有过期时间，写入 TTL marker
    if (has_ttl) {
        write_uint8(static_cast<uint8_t>(RdbSpecialMarker::KV_WITH_TTL));
        write_uint64(static_cast<uint64_t>(expire_time_ms));
    }

    // 写入 key
    write_string(key);

    // 写入 value type
    RdbValueType type;
    switch (value.type()) {
        case ObjectType::STRING: type = RdbValueType::STRING; break;
        case ObjectType::LIST: type = RdbValueType::LIST; break;
        case ObjectType::HASH: type = RdbValueType::HASH; break;
        case ObjectType::SET: type = RdbValueType::SET; break;
        case ObjectType::ZSET: type = RdbValueType::ZSET; break;
        default: type = RdbValueType::STRING; break;
    }
    write_uint8(static_cast<uint8_t>(type));

    // 序列化 value
    switch (value.type()) {
        case ObjectType::STRING: serialize_string(value.get_string().value_or("")); break;
        case ObjectType::LIST: serialize_list(value); break;
        case ObjectType::HASH: serialize_hash(value); break;
        case ObjectType::SET: serialize_set(value); break;
        case ObjectType::ZSET: serialize_zset(value); break;
        default: break;
    }
}

bool RdbPersistence::read_kv_pair(std::string& key, CacheObject& value, int64_t& expire_time_ms) {
    // 初始化为永不过期
    expire_time_ms = -1;

    // 检查是否有过期时间标记
    long marker_pos = ftell(file_);
    uint8_t marker = read_uint8();

    if (marker == static_cast<uint8_t>(RdbSpecialMarker::KV_WITH_TTL)) {
        // 读取过期时间戳（毫秒）
        uint64_t expire = read_uint64();
        expire_time_ms = static_cast<int64_t>(expire);
        LOG_DEBUG(RDB, "Reading KV with TTL: expire=%ld", expire_time_ms);
    } else if (marker_pos >= 0) {
        // 没有 TTL，恢复文件位置到 marker 之前
        fseek(file_, marker_pos, SEEK_SET);
    }

    // 读取 key
    key = read_string();
    if (key.empty()) return false;

    // 读取 value type
    uint8_t type_val = read_uint8();
    RdbValueType type = static_cast<RdbValueType>(type_val);

    bool success = true;
    switch (type) {
        case RdbValueType::STRING: success = deserialize_string(value); break;
        case RdbValueType::LIST: success = deserialize_list(value); break;
        case RdbValueType::HASH: success = deserialize_hash(value); break;
        case RdbValueType::SET: success = deserialize_set(value); break;
        case RdbValueType::ZSET: success = deserialize_zset(value); break;
        default:
            LOG_WARN(RDB, "Unknown value type: %u", type_val);
            success = false;
    }

    return success;
}

void RdbPersistence::serialize_string(const std::string& val) {
    write_string(val);
}

void RdbPersistence::serialize_list(const CacheObject& obj) {
    auto items = obj.list_range(0, -1);
    write_uint32(static_cast<uint32_t>(items.size()));
    for (const auto& item : items) {
        write_string(item);
    }
}

void RdbPersistence::serialize_hash(const CacheObject& obj) {
    auto items = obj.hash_items();
    write_uint32(static_cast<uint32_t>(items.size()));
    for (const auto& [k, v] : items) {
        write_string(k);
        write_string(v);
    }
}

void RdbPersistence::serialize_set(const CacheObject& obj) {
    auto members = obj.set_members();
    write_uint32(static_cast<uint32_t>(members.size()));
    for (const auto& member : members) {
        write_string(member);
    }
}

void RdbPersistence::serialize_zset(const CacheObject& obj) {
    auto members = obj.zset_all();
    write_uint32(static_cast<uint32_t>(members.size()));
    for (const auto& [member, score] : members) {
        write_string(member);
        // double 转 bits 再写入（网络字节序）- 使用 memcpy 避免 UB
        uint64_t score_bits;
        std::memcpy(&score_bits, &score, sizeof(score_bits));
        write_uint64(score_bits);
    }
}

bool RdbPersistence::deserialize_string(CacheObject& obj) {
    std::string val = read_string();
    obj = CacheObject(val);
    return true;
}

bool RdbPersistence::deserialize_list(CacheObject& obj) {
    uint32_t count = read_uint32();
    for (uint32_t i = 0; i < count; ++i) {
        std::string item = read_string();
        obj.list_push(item, false);  // RPUSH 方式
    }
    return true;
}

bool RdbPersistence::deserialize_hash(CacheObject& obj) {
    uint32_t count = read_uint32();
    for (uint32_t i = 0; i < count; ++i) {
        std::string field = read_string();
        std::string value = read_string();
        obj.hash_set(field, value);
    }
    return true;
}

bool RdbPersistence::deserialize_set(CacheObject& obj) {
    uint32_t count = read_uint32();
    for (uint32_t i = 0; i < count; ++i) {
        std::string member = read_string();
        obj.set_add(member);
    }
    return true;
}

bool RdbPersistence::deserialize_zset(CacheObject& obj) {
    uint32_t count = read_uint32();
    for (uint32_t i = 0; i < count; ++i) {
        std::string member = read_string();
        uint64_t score_bits = read_uint64();
        // bits 转 double - 使用 memcpy 避免 UB
        double score;
        std::memcpy(&score, &score_bits, sizeof(score));
        obj.zset_add(member, score);
    }
    return true;
}

uint32_t RdbPersistence::calculate_crc32_for_range(long end_pos) {
    // 计算范围：[0, end_pos)，即排除 end_pos 本身

    // 保存当前文件指针位置
    long current_pos = ftell(file_);

    long crc_len = end_pos;
    fseek(file_, 0, SEEK_SET);

    // 检查是否有错误
    if (ferror(file_)) {
        LOG_ERROR(RDB, "calculate_crc32_for_range: ferror before fread, current_pos=%ld", current_pos);
        clearerr(file_);
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    uint8_t buf[4096];

    size_t total_read = 0;
    while (crc_len > 0) {
        size_t to_read = static_cast<size_t>(std::min(crc_len, static_cast<long int>(sizeof(buf))));
        size_t len = fread(buf, 1, to_read, file_);
        if (len == 0) {
            if (feof(file_)) {
                LOG_ERROR(RDB, "calculate_crc32_for_range: EOF at position %zu, crc_len=%ld", total_read, crc_len);
            } else if (ferror(file_)) {
                LOG_ERROR(RDB, "calculate_crc32_for_range: fread error at position %zu", total_read);
                clearerr(file_);
            }
            break;
        }
        crc = crc32(crc, buf, static_cast<uInt>(len));
        crc_len -= static_cast<long>(len);
        total_read += len;
    }

    LOG_INFO(RDB, "calculate_crc32_for_range: end_pos=%ld, total_read=%zu, crc=0x%08X",
             end_pos, total_read, crc);

    // 恢复文件指针位置
    fseek(file_, current_pos, SEEK_SET);

    return static_cast<uint32_t>(crc);
}

}  // namespace cc_server