# 持久化架构

> **范围**：RDB 快照格式、5 类型序列化/反序列化、RdbScheduler 自动保存、优雅退出保存。
> **源码**：`src/persistence/`
> **前置阅读**：[架构总览](./overview.md) · [存储层](./storage.md)

## 1. 设计目标

| 目标 | 手段 |
|------|------|
| 服务重启不丢数据 | RDB 周期快照 + 启动 `load()` 恢复 |
| 不阻塞主线程 | 后台线程 `RdbScheduler` + 进程内后台线程快照（Windows 平台为子进程） |
| 保存原子性 | 先写 `.tmp` 临时文件 → fsync → `rename()` 原子替换（见 §5.1） |
| 5 数据类型全支持 | STRING/LIST/HASH/SET/ZSET 各自独立序列化方法 |
| 写后容灾 | TTL 信息随快照持久化 |
| 优雅退出不丢最后写 | `main.cpp` 退出路径强制 `save()` |

## 2. RDB 文件格式

### 2.1 魔数与版本

```cpp
constexpr uint32_t kRdbMagic   = 0x43435244;  // "CCRD"  (大端)
constexpr uint8_t  kRdbVersion[4] = {'0','0','0','2'};
```

### 2.2 帧结构

```mermaid
flowchart LR
    HDR[Header<br/>magic 4B + version 4B<br/>+ db_count 4B + kv_count 4B] --> KVS[KV 序列]
    KVS --> EOF[EOF marker 0xFF]
    EOF --> CRC[CRC32 4B]

    subgraph KVS[KV 条目（重复 kv_count 次）]
        T{有 TTL?}
        T -->|Yes| TTM[KV_WITH_TTL 0xFE + 绝对过期时间戳 8B<br/>→ key_len 4B + key → Type 1B → value]
        T -->|No| NK[key_len 4B + key → Type 1B → value]
    end
```

> 加载端是**计数驱动**：先读 `db_count`、`kv_count`，按 `kv_count` 循环读取 KV 条目，最后读 EOF marker——不依赖 EOF 来终止循环。CRC 在循环**之前**就验完（覆盖 `[0, size-4)`），所以一个被截断或改坏的文件不会先把半个数据集留在内存里；循环结束后还要求游标正好落在 EOF marker 处，正文比头部声称的多一条少一条都判坏。
>
> 每个 `*_len` 字段都先跟正文真正还剩的字节数比一遍，才拿去分配内存，所以一个几十字节的 `dump.rdb` 把 `len` 写成 `0xFFFFFFFF` 也换不来一次 4GB 的申请。
>
> 字段顺序固定为「TTL marker（可选，最前）→ key → type → value」。`KV_WITH_TTL` 中的 8 字节是**绝对过期时间戳**（epoch ms），不是剩余 TTL。

### 2.3 类型字节（`RdbValueType`）

| 值 | 含义 | 底层序列化 |
|----|------|-----------|
| `0x00` | STRING | `string_len(4B) + bytes` |
| `0x01` | LIST | `count(4B) + count × (len + bytes)` |
| `0x02` | HASH | `count(4B) + count × (key_len + key + val_len + val)` |
| `0x03` | SET | `count(4B) + count × (len + bytes)` |
| `0x04` | ZSET | `count(4B) + count × (member_len + member + score_bits 8B，IEEE-754 大端)` |
| `0xFE` | KV_WITH_TTL（前置 marker） | `绝对过期时间戳 epoch ms(8B)` |
| `0xFF` | EOF_MARKER | 循环后读取；游标必须正好停在它前面，否则判坏 |

## 3. 核心类

### 3.1 `RdbPersistence`（`src/persistence/rdb.{h,cpp}`）

| 成员 | 类型 | 说明 |
|------|------|------|
| `static RdbPersistence& instance()` | Magic Static | 全局单例 |
| `file_` | `FILE*` | 当前打开的 RDB 文件 |
| `filepath_` | `string` | RDB 路径（`rdb_path` 配置） |
| `bgsave_in_progress_` | `atomic<int>` | BGSAVE 是否正在进行 |
| `stats_` | `RdbStats` | 统计信息（原子字段） |
| `save_mutex_` | `std::mutex` | 串行化同步 save 与后台 save，防止并发写同一文件 |
| `save(filepath, storage)` | `bool` | 同步保存（阻塞；原子写：.tmp + fsync + rename，见 §5.1） |
| `save_in_background(filepath, storage)` | `bool` | 异步保存（进程内 detached 线程） |
| `load(filepath, storage)` | `bool` | 启动时加载 |
| `wait_for_bgsave(timeout_ms)` | `bool` | 等待 BGSAVE 完成 |

**`RdbStats`**：

```cpp
struct RdbStats {
    std::atomic<int64_t>    last_bgsave_time_sec{0};
    std::atomic<BgsaveStatus> last_bgsave_status{BgsaveStatus::IDLE};
    std::atomic<size_t>     last_bgsave_keys{0};
    std::atomic<size_t>     total_bgsave_calls{0};
    std::atomic<size_t>     total_rdb_saved_keys{0};
};
```

### 3.2 `RdbScheduler`（`src/persistence/rdb_scheduler.{h,cpp}`）

| 成员 | 类型 | 说明 |
|------|------|------|
| `storage_` | `GlobalStorage&` | 被持久化的对象 |
| `rdb_path_` | `string` | 目标路径 |
| `config_` | `SaveConfig` | 调度策略 |
| `scheduler_thread_` | `thread` | 后台调度线程 |
| `running_` | `atomic<bool>` | 运行标志 |

**`SaveConfig`**：

```cpp
struct SaveConfig {
    int interval_sec    = 900;   // 默认 15 分钟
    int dirty_threshold = 1;     // 默认 1 个脏键就触发
};
```

> 实际取值由 `conf/concurrentcache.conf` 的 `rdb_save_interval` / `rdb_dirty_threshold` 覆盖。注意：当前 conf 文件中 `rdb_save_interval = 0` 属非法值（main 会回退为默认 900）；`rdb_dirty_threshold` 未在 conf 中配置（走默认 1）。

## 4. 调度策略

```mermaid
flowchart TB
    S[scheduler thread loop] --> SL[std::this_thread::sleep_for 1s]
    SL --> CK{条件判断}
    CK --> C1{interval_sec<br/>已到?}
    CK --> C2{dirty_counter<br/>>= threshold?}
    C1 -->|Yes| DO[do_save]
    C2 -->|Yes| DO
    C1 -->|No| SL
    C2 -->|No| SL
    DO --> BG[save_in_background<br/>进程内 detached 线程]
    BG --> RES[成功启动后 CAS<br/>consume_dirty_count]
    RES --> SL
    BG --> BT[后台线程 save<br/>完成后自行更新 stats]
```

**do_save 流程**（`rdb_scheduler.cpp::do_save()`）：

```text
1. if rdb.is_bgsave_in_progress(): return   // 防并发
2. rdb.save_in_background(rdb_path, storage)  // 启动后台线程
3. 启动成功 → 立即 storage.consume_dirty_count(dirty_snapshot)
   （CAS 扣掉【保存启动时刻】已观测到的那部分，而非完成时刻整体清零——
    否则 threshold==1 时每轮调度都会重复触发 BGSAVE，且启动期间的新写入会被漏计）
4. stats 由后台线程保存完成后自行更新
```

## 5. 同步保存 vs 异步保存

| 维度 | `save` (同步) | `save_in_background` (异步) |
|------|---------------|---------------------------|
| 调用方 | `SAVE` 命令、优雅退出 | `BGSAVE` 命令、`RdbScheduler` |
| 阻塞主线程 | **是** | 否（后台 detached 线程） |
| 内存峰值 | 高（`get_all_objects_with_ttl()` 一次性深拷贝全库，持各分片 shared_lock） | 同左（后台线程内同样深拷贝） |
| 失败处理 | 返回错误给客户端 | 更新 `last_bgsave_status` |
| 实现 | `RdbPersistence::save` | 内部调用 `save`，在 detached 线程执行 |

> **为什么不用 fork？** 多线程进程下 `fork()` 有死锁/UB 风险（子进程可能复制到持锁状态的堆）（子进程可能复制到持锁状态的堆），因此**刻意**改为进程内后台线程快照，仅 Windows 分支保留子进程方案。代价是后台线程与写请求竞争分片锁（大库保存期间写延迟可能上升）。

### 5.1 原子保存流程（`save()` 内部）

```text
1. std::lock_guard(save_mutex_)          // 串行化所有保存
2. 以 filepath + ".tmp" 写入全部数据
   ├─ fflush + fsync(fileno)             // 刷用户态/内核态缓冲
   └─ 任一步失败 → 抛异常 → std::remove(tmp_path) 清理 → 返回失败
3. rename(tmp_path, filepath)            // 原子替换目标文件；失败同样清理 tmp
4. fsync 目标目录（open(dir, O_RDONLY) + fsync + close）
   // 持久化 rename 元数据，防止掉电后 rename 丢失
```

> **为什么不直接写目标文件？** 写一半崩溃会留下半截 RDB，下次启动 `load()` 失败丢全部数据。`.tmp + rename` 保证磁盘上永远是完整文件或旧文件。

## 6. 5 数据类型序列化

### 6.1 STRING

```cpp
void RdbPersistence::write_kv_pair(key, CacheObject obj, expire_time_ms) {
    if (expire_time_ms > 0) {
        write_uint8(KV_WITH_TTL);            // 0xFE，前置 marker
        write_uint64(expire_time_ms);        // 绝对过期时间戳 epoch ms
    }
    write_uint32(key.size()); write_string(key);
    write_uint8(RdbValueType::STRING);       // 0x00
    write_uint32(obj.get_string()->size());
    write_string(obj.get_string().value());
}
```

### 6.2 LIST

```cpp
void serialize_list(const CacheObject& obj) {
    write_uint8(LIST);                    // 0x01
    auto& list = obj.list_val_;
    write_uint32(list.size());
    for (auto& item : list) {
        write_uint32(item.size());
        write_string(item);
    }
}
```

### 6.3 HASH

```cpp
void serialize_hash(const CacheObject& obj) {
    write_uint8(HASH);                    // 0x02
    write_uint32(obj.hash_val_.size());
    for (auto& [k, v] : obj.hash_val_) {
        write_uint32(k.size()); write_string(k);
        write_uint32(v.size()); write_string(v);
    }
}
```

### 6.4 SET

```cpp
void serialize_set(const CacheObject& obj) {
    write_uint8(SET);                     // 0x03
    write_uint32(obj.set_val_.size());
    for (auto& m : obj.set_val_) {
        write_uint32(m.size()); write_string(m);
    }
}
```

### 6.5 ZSET

```cpp
void serialize_zset(const CacheObject& obj) {
    write_uint8(ZSET);                    // 0x04
    write_uint32(obj.zset_val_.size());
    for (auto& m : obj.zset_val_) {       // 已有序
        write_uint32(m.member.size()); write_string(m.member);
        uint64_t score_bits;
        std::memcpy(&score_bits, &m.score, sizeof(score_bits));
        write_uint64(score_bits);         // IEEE-754 bits，大端网络字节序
    }
}
```

**反序列化**：每个类型对应一个 `deserialize_xxx(CacheObject&)` 方法，按相同顺序读回。

## 7. 加载流程

`main.cpp` 在 `SubReactorPool.start()` 之后、`ExpirationChecker.start()` 之前调用 `RdbPersistence::load(rdb_path, storage)`。

```mermaid
sequenceDiagram
    participant M as main()
    participant RP as RdbPersistence
    participant GS as GlobalStorage
    M->>RP: load(path, storage)
    RP->>RP: fopen(path, "rb")
    RP->>RP: 0 字节文件 → 当作空数据集，直接返回成功
    RP->>RP: read magic == "CCRD"? 不符 → fail
    RP->>RP: read version == "0002"? 不符 → 拒绝加载
    RP->>RP: 跳到文件尾读 CRC32，与 [0, size-4) 现算的比对 → 不符 → fail（此时一个字节都还没进存储）
    RP->>RP: read db_count
    RP->>RP: read kv_count
    loop kv_count 次
        RP->>RP: opt type == 0xFE → read 绝对过期时间戳
        RP->>RP: read key → read type → deserialize_xxx（未知 type → 抛异常，整份文件判坏）
        alt expire_time_ms > 0 且 ttl = expire - now > 0
            RP->>GS: storage.set_with_expire(key, obj, 剩余ttl_ms)
        else 已过期
            RP->>RP: 跳过该 key，不加载
        else 无 TTL
            RP->>GS: storage.set(key, obj)
        end
    end
    RP->>RP: 游标必须正好停在 EOF marker 前一个字节 → 不对 → fail
    RP->>RP: read EOF marker == 0xFF? 不符 → fail
    RP->>RP: fclose
```

**关键不变量**：

- 加载过程中不启动 `ExpirationChecker`（避免并发修改 `GlobalStorage`）
- 加载顺序与持久化时一致（保证 ZSet 等有序结构正确）
- **版本不匹配直接拒绝加载**（返回 false），不部分加载
- **已过期的 key 加载时跳过**（按剩余 TTL 原子写回，`set_with_expire` 同时维护 `expire_dict_` 与 `CacheEntry::expire_at_ms`）
- 加载失败不致命（打印警告，从空存储启动）

## 8. 关键不变量

| 不变量 | 维护机制 |
|--------|---------|
| 单实例 | Magic Static + `delete` 拷贝 |
| BGSAVE 不并发 | `bgsave_in_progress_` 原子标志（SAVE/BGSAVE 命令均先检查） |
| 同步/异步保存不并发写文件 | `save_mutex_` 串行化 |
| 磁盘上永远是完整文件 | `.tmp` 写入 + fsync + 原子 `rename`（§5.1） |
| 写后脏计数递增 | `set/del/set_with_expire/incrby` 在 `GlobalStorage` 内部递增（注意：EXPIRE 命令直改 `expire_dict_` 的路径**不**递增脏计数） |
| 脏计数扣减时机 | BGSAVE **启动成功时**用 `consume_dirty_count(观测值)` 扣减（而非完成时清零），防 threshold==1 时每轮重复触发，也不吞掉启动期间的新写入 |
| 启动前已恢复 | `load()` 在 `SubReactor.start()` 之后、`ExpirationChecker.start()` 之前 |
| 优雅退出保存 | `main.cpp` 关闭流程最后 `rdb.save(path, storage)` |
| 5 类型全支持 | `RdbValueType` 枚举 + 各自序列化方法 |
| TTL 持久化 | `KV_WITH_TTL` marker + 8 字节**绝对过期时间戳**（epoch ms） |
| 序列化顺序 = 反序列化顺序 | 严格 `for-each` 写入 / `for-each` 读取 |

## 9. 性能与调优

| 现象 | 调优点 |
|------|-------|
| RDB 文件过大 | 调小 `rdb_save_interval` 更频繁落盘 |
| 启动加载慢 | 减少 `max_entries` 或分批 `set` |
| 写后磁盘压力大 | 调高 `rdb_dirty_threshold` |
| BGSAVE 内存峰值 | 限制单实例 `max_entries` ≤ 1M |
| 加载后数据错乱 | 检查文件 magic / CRC |

## 10. 关键源码位置

| 关注点 | 文件 |
|--------|------|
| 同步 save（含原子写流程） | `src/persistence/rdb.cpp`（`RdbPersistence::save`） |
| 异步 save（后台线程） | `src/persistence/rdb.cpp`（`save_in_background`） |
| 加载 | `src/persistence/rdb.cpp`（`RdbPersistence::load`） |
| 调度循环 | `src/persistence/rdb_scheduler.cpp`（`schedule_loop/do_save`） |
| 5 类型序列化 | `src/persistence/rdb.cpp`（`serialize_string/list/hash/set/zset`） |

## 11. 另见

- [存储层 § 6 RDB 集成](./storage.md#6-rdb-集成)
- [部署 § 持久化策略](../deployment.md)
- [API § SAVE / BGSAVE](../api.md)
