# 测试文档

> **测试体系**：C++ 单元/集成测试 + Python E2E 测试（含对比测试）
> **入口**：`test/CMakeLists.txt`（C++ 测试目标） + `test/e2e_test/run_all_tests.py`（E2E 驱动）
> **测试统计**：12 个 C++ 可执行文件 + 11 个 Python 脚本

## 1. 概览

| 层次 | 工具 | 范围 |
|------|------|------|
| **单元测试** | C++ + 自研轻量框架（`test/trace/test_assertions.h`） | 单组件正确性（atomic、lock、object、container） |
| **集成测试** | C++ + CTest | 模块间协作（storage ↔ expire、rdb ↔ storage） |
| **端到端** | Python 3.7+（原生 socket，无外部依赖） | 真实场景下并发 / 容错 / 混沌 |
| **对比测试** | Python 3.7+ | ConcurrentCache vs Redis 功能/性能/鲁棒性对比 |
| **压力测试** | C++ 长跑 + Python 极限探针 | QPS / 延迟 / 资源占用基线 |

## 2. 测试可执行文件清单

定义于 `test/CMakeLists.txt`：

| 可执行文件 | 源码 | 测试重点 |
|-----------|------|---------|
| `concurrentcache-v3-tests` | `test_v3_main.cpp` + `datatype_test/` + `persistence_test/` + `storage_test/` | V3 综合：5 数据类型 / RDB / 64 分片存储 |
| `atomic-tests` | `atomic_test/atomic_correctness_test.cpp` | `std::atomic` 内存序、fetch_add、compare_exchange |
| `lock-correctness-tests` | `lock_test/lock_correctness_test.cpp` | Mutex / SpinLock / RWLock 互斥语义 |
| `lock-deadlock-tests` | `lock_test/lock_deadlock_test.cpp` | 死锁检测 / try_lock / 嵌套加锁 |
| `lock-race-tests` | `lock_test/lock_race_test.cpp` | 高并发竞态下数据一致性 |
| `lock-boundary-tests` | `lock_test/lock_boundary_test.cpp` | 空容器、极端并发数、长时间持锁 |
| `sync-primitives-tests` | `sync_primitives_test/sync_primitives_test.cpp` | CountDownLatch / CyclicBarrier / Semaphore |
| `stress-test` | `stress_test/stress_test.cpp` | 高并发读写混合（短期） |
| `long-running-stress-test` | `stress_test/long_running_stress_test.cpp` | 长时间稳定性（数小时） |
| `load-limit-test` | `stress_test/load_limit_test.cpp` | 逐步加压找性能拐点 |
| `network-stress-test` | `network_test/network_stress_test.cpp` | SubReactorPool 大连接并发 |
| `cluster-tests` | `cluster_test/cluster_test.cpp` + `cluster_replication_test.cpp` + `cluster_strict_test.cpp` | 集群 Gossip / 复制 / 槽位 |

> **注意**：`command_test/` 目录源码已就绪但**当前 `test_v3_main.cpp` 中禁用**。启用方法见 § 7。

## 3. 快速运行

### 3.1 编译

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

### 3.2 跑全部 C++ 测试

```bash
ctest --test-dir build --output-on-failure
```

> 当前 CTest 注册项：`AtomicTests`、`LockCorrectnessTests`、`LockDeadlockTests`、`LockRaceTests`、`ClusterTests`。其他可执行文件需手动运行。

### 3.3 单独跑 V3 综合测试

```bash
./build/test/concurrentcache-v3-tests                       # 全部
./build/test/concurrentcache-v3-tests --storage             # 仅存储层
./build/test/concurrentcache-v3-tests --datatype            # 仅数据类型
./build/test/concurrentcache-v3-tests --rdb                 # 仅 RDB
./build/test/concurrentcache-v3-tests --help                # 帮助
```

### 3.4 跑压力测试

```bash
./build/test/stress-test                    # 短期压力
./build/test/load-limit-test                # 找极限
./build/test/long-running-stress-test &     # 长期后台
./build/test/network-stress-test            # 网络层压力
```

### 3.5 跑 E2E 测试

```bash
# 先启动服务
./build/concurrentcache-server &

# 单项
python3 test/e2e_test/e2e_connection_storm.py
python3 test/e2e_test/e2e_high_concurrency_load.py --users 1000
python3 test/e2e_test/e2e_consistency_check.py
python3 test/e2e_test/e2e_chaos_test.py
python3 test/e2e_test/e2e_failover_test.py
python3 test/e2e_test/e2e_cluster_full_test.py
python3 test/e2e_test/e2e_psync_replication_test.py
python3 test/e2e_test/stress_find_limit.py

# 一键运行
python3 test/e2e_test/run_all_tests.py
```

**前置**：

```bash
ulimit -n 65535   # 提升 fd 上限（连接洪峰测试必需）
```

### 3.6 跑 Redis 对比测试

```bash
# 需要 Redis 7.0.15 可执行
python3 test/e2e_test/comparison_test.py
```

> 对比测试会同时启动 ConcurrentCache 和 Redis，进行 4 部分深度对比：
> 1. 功能正确性（39 项）
> 2. 性能基准（多并发档 + 大 Value）
> 3. 极限/鲁棒性
> 4. 内存占用
> 结果输出 JSON 报告和终端表格。

## 4. 断言与测试工具

`test/trace/test_assertions.h` 自研轻量框架：

| 宏 | 失败时 |
|---|--------|
| `EXPECT_TRUE/FALSE/EQ/NE/LT/LE/GT/GE` | 继续执行 |
| `EXPECT_TRUE_TIMEOUT(cond, ms)` | 超时则失败 |
| `ASSERT_TRUE/...` | **立即终止当前套件** |
| `SKIP()` | 计入 skipped |
| `TEST_SUITE("name")` | 套件，自动统计 |

**示例**：

```cpp
TEST_SUITE("GlobalStorage Basic Operations") {
    EXPECT_EQ(GlobalStorage::instance().size(), 0);
    GlobalStorage::instance().set("k", CacheObject("v"));
    EXPECT_TRUE(GlobalStorage::instance().exist("k"));
    auto v = GlobalStorage::instance().get("k");
    EXPECT_TRUE(v.has_value());
}
```

**输出**：

```text
========================================
  Test Suite: GlobalStorage Basic Operations
========================================
  Results for: GlobalStorage Basic Operations
    Total:  8
    Passed: 8
    Failed: 0
    Skipped: 0
========================================
```

**全局统计**：`g_test_stats()` 单例，所有断言自动累加 PASS/FAIL/SKIP。

## 5. C++ 测试套件详解

### 5.1 存储层（`concurrentcache-v3-tests --storage`）

**源文件**：`test/storage_test/storage_v3_test.cpp`

| 套件 | 覆盖点 |
|------|--------|
| `GlobalStorage Basic Operations` | SET / GET / DEL / EXISTS / SIZE |
| `GlobalStorage TTL` | 相对过期、惰性删除、过期后不可读 |
| `GlobalStorage Set Expire Time` | 绝对时间戳设置 |
| `GlobalStorage Set With Expire` | 原子设置值+过期 |
| `GlobalStorage Persist` | 移除过期时间 |
| `GlobalStorage Dirty Counter` | 脏计数（用于 RDB 触发判断） |
| `GlobalStorage Concurrent Read Write` | 8 线程 × 1000 操作并发一致性 |
| `GlobalStorage Sharding Performance` | 8 线程插入 10000 key |
| `GlobalStorage Large Dataset` | 10 万 key 插入与随机读 |
| `GlobalStorage Multiple DataTypes` | 5 种类型混合存储 |
| `GlobalStorage Get All Objects` | 全量导出（RDB 前置步骤） |

### 5.2 数据类型（`concurrentcache-v3-tests --datatype`）

**源文件**：`test/datatype_test/object_test.cpp`

- String：set/get
- List：push/pop/range/len
- Hash：set/get/del/len/items
- Set：add/contains/size/members/remove
- ZSet：add/score/card/range/remove

### 5.3 RDB 持久化（`concurrentcache-v3-tests --rdb`）

**源文件**：`test/persistence_test/rdb_test.cpp`

- 5 类型序列化 / 反序列化
- 10 万 key 快照与恢复
- 空数据库、空 value、特殊字符

### 5.4 锁机制（4 个独立可执行文件）

| 可执行 | 源文件 | 测试重点 |
|--------|--------|---------|
| `lock-correctness-tests` | `lock_test/lock_correctness_test.cpp` | 基本加锁/解锁、互斥语义 |
| `lock-deadlock-tests` | `lock_test/lock_deadlock_test.cpp` | 死锁检测、try_lock、嵌套加锁顺序 |
| `lock-race-tests` | `lock_test/lock_race_test.cpp` | 高并发竞态下数据一致性 |
| `lock-boundary-tests` | `lock_test/lock_boundary_test.cpp` | 边界条件、长时间持锁 |

**额外文件**：`lock_test/lock_stress_test.cpp`（长期重负载稳定性）。

**通过标准**：
- `lock-race`：100 万次并发操作后数据完全一致
- `lock-deadlock`：死锁场景正确报错或回退，无进程挂起
- `lock-boundary`：无死锁、无 panic

### 5.5 同步原语

**可执行文件**：`sync-primitives-tests`

- CountDownLatch 计数准确性
- CyclicBarrier 多轮同步
- Semaphore 资源池限流

### 5.6 原子操作

**可执行文件**：`atomic-tests`（含 5 个子文件）

| 子文件 | 测试内容 |
|--------|---------|
| `atomic_first_test.cpp` | 基础 fetch_add / compare_exchange |
| `atomic_minimal_test.cpp` | 最小可重现样例 |
| `atomic_progressive_test.cpp` | 渐进式复杂场景 |
| `atomic_multi_test.cpp` | 多线程竞争场景 |
| `atomic_memory_order_test.cpp` | memory_order_relaxed / acquire / release |

### 5.7 网络压测

**可执行文件**：`network-stress-test`

- 数万并发连接建立/关闭
- 短连接 vs 长连接混合
- 慢客户端（slow consumer）场景

### 5.8 性能压测（3 个独立可执行文件）

| 可执行 | 测试重点 | 时长 |
|--------|---------|------|
| `stress-test` | 高并发读写混合 | 数十秒 |
| `long-running-stress-test` | 长时间稳定性 | 数小时 |
| `load-limit-test` | 逐步加压找性能拐点 | 数分钟 |

### 5.9 集群

**可执行文件**：`cluster-tests`（含 3 个源文件）

- 节点 Gossip 心跳
- 哈希槽分配
- PSYNC 握手
- 复制偏移量
- 故障检测

## 6. Python E2E 脚本

| 脚本 | 目的 | 时长 |
|------|------|------|
| `comparison_test.py` | **ConcurrentCache vs Redis 深度对比**（功能/性能/鲁棒性/内存） | ~5 分钟 |
| `e2e_connection_storm.py` | 10000+ 并发连接洪峰 | ~1 分钟 |
| `e2e_high_concurrency_load.py` | 1000 虚拟用户压测 | 数十秒 |
| `e2e_consistency_check.py` | 多协程竞态一致性 | 数十秒 |
| `e2e_chaos_test.py` | 异常 / 混沌 / 恶意输入 | ~1 分钟 |
| `e2e_failover_test.py` | 主从故障转移 | ~2 分钟 |
| `e2e_cluster_full_test.py` | 集群全功能 | 数十秒 |
| `e2e_psync_replication_test.py` | PSYNC 复制验证 | 数十秒 |
| `cluster_stress_test.py` | 集群压力 | 数分钟 |
| `stress_find_limit.py` | 寻找性能极限 | 数分钟 |
| `run_all_tests.py` | 上述脚本总入口（不含 `comparison_test.py`） | 取决于组合 |

> `comparison_test.py` 需要安装 `redis`，其余脚本使用原生 Python socket（无外部依赖）。

**典型用法**：

```bash
# 对比测试
python3 test/e2e_test/comparison_test.py

# 其他 E2E 测试
python3 test/e2e_test/e2e_connection_storm.py --connections 10000 --batch-size 500
python3 test/e2e_test/e2e_high_concurrency_load.py --users 1000 --commands 100
python3 test/e2e_test/e2e_chaos_test.py
```

**混沌测试覆盖**：

1. TCP Half-Close（RST 断开）
2. 巨大 Payload（50MB）
3. 畸形 RESP 协议
4. 快速连接断开（1000 连接/秒）
5. 并发异常请求

## 7. 启用已禁用的 command_test

源码在 `test/command_test/`，**当前 `test_v3_main.cpp` 注释了 `run_all_command_tests()` 调用**。

启用步骤：

1. 编辑 `test/test_v3_main.cpp`，取消注释 `run_all_command_tests()` 及其声明
2. 在 `test/CMakeLists.txt` 的 `concurrentcache-v3-tests` 目标中添加 `command_test/command_test.cpp`
3. 重新 `cmake --build build`

## 8. Sanitizer 使用

```bash
# ASan（内存越界 / UAF）— Debug 构建
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build

# TSan（数据竞争）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON && cmake --build build

# UBSan（未定义行为）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON && cmake --build build
```

> 三种 Sanitizer **不可同时启用**。

| Sanitizer | 典型报告 | 排查 |
|-----------|---------|------|
| ASan | `heap-buffer-overflow` / `use-after-free` | 堆越界 / 释放后使用 |
| TSan | `data race` | 并发数据竞争 |
| UBSan | `undefined behavior` | UB：符号溢出、空指针解引用等 |

## 9. CI 集成

文件：`.github/workflows/ci.yml`

- **触发**：push / PR 到 `main` / `master`
- **环境**：`ubuntu-24.04` + `cmake` + `build-essential` + `zlib1g-dev`
- **步骤**：`cmake .. -DCMAKE_BUILD_TYPE=Release` → `cmake --build build --parallel 2` → 上传 binary artifact（保留 1 天）

> 当前 CI **仅构建**，不运行测试套件。回归测试需在本地或 PR 审查时手动执行。

## 10. 故障排查

| 症状 | 排查 |
|------|------|
| `undefined reference to cc_server::*` | 检查 `test/CMakeLists.txt` 的 `COMMON_SOURCES` 是否包含新增源 |
| `zlib not found` | `apt install zlib1g-dev` |
| `C++20 features not supported` | 升级 GCC ≥ 12 / Clang ≥ 16 |
| 死锁/挂起 | `gdb -p <pid>` → `thread apply all bt` |
| `ConnectionRefusedError` (E2E) | 服务端未启动或端口不对 |
| `too many open files` | `ulimit -n 65535` |
| 大量 timeout | 减小 `--users` / `--commands` |

## 11. 另见

- [架构总览 § 7 性能特征](architecture/overview.md)
- [部署 § 2.4 Sanitizer 选项](../deployment.md)
- [API 文档](../api.md) — 命令级测试输入
