# 测试文档

> **测试体系**：C++ 单元/集成测试 + Python E2E 测试（含对比测试）
> **入口**：`test/CMakeLists.txt`（C++ 测试目标） + `test/e2e_test/run_all_tests.py`（E2E 驱动）
> **测试统计**：17 个 C++ 可执行 target（其中 16 个登记进 CTest，另有 1 个是给一致性脚本用的 `command-table-probe` 探针）+ 12 个 Python 脚本

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
| `network-stress-test` | `network_test/network_stress_test_main.cpp` + `network_stress_test.cpp` | SubReactorPool 大连接并发 |
| `cluster-tests` | `cluster_test/cluster_test.cpp` | 集群 Gossip / 复制 / 槽位（纯对象级，不起 socket） |
| `cluster-replication-tests` / `cluster-strict-tests` | **未接入构建** | 见下方说明 |
| `lock-stress-tests` | `lock_test/lock_stress_test.cpp` | 锁的高强度混压（`slow`，只在 daily 跑） |
| `concurrency-tests` | `test_main.cpp` + `tests.cpp` | 线程池 / 分片锁 / 内存池三层一致性 |
| `command-table-probe` | `tools/command_table_probe.cpp` | 不是用例：给 CI 的一致性检查当运行时注册表（`CommandFactory::create()`） |

> **有源码但故意不接入构建**（`ci/known-failures.txt` 里逐条登记，脚本会检查"test/ 下的 .cpp 必须属于某个 target 或在名单里"，所以新增孤儿文件会当场红）：
>
> | 文件 | 不接入的原因 |
> |------|--------------|
> | `command_test/command_test.cpp` | 调用已不存在的 `CommandFactory(GlobalStorage&)` 与 `create_command()`；且框架 `expect_eq` 是单模板参数，`EXPECT_EQ(std::string, "字面量")` 推导不出来 |
> | `cluster_test/cluster_replication_test.cpp` | 调用 `ReplicationMgr::add_to_replication_buffer()`，而它在 `replication_mgr.h:128` 之后是 private |
> | `cluster_test/cluster_strict_test.cpp` | 同上；另有 `getNodeByIpPort("", 6379)` 会在 Debug 触发 `cluster_state.cpp:94` 的 assert |
> | `atomic_test/atomic_{first,minimal,multi,progressive,memory_order}_test.cpp` | 各自带 `main()` 的历史复现脚本，与已接入的 `atomic_correctness_test.cpp` 互斥 |
>
> 这些文件需要**重写**而不是"接线"，属于缺陷队列。

## 2.1 CI 分层与基线棘轮

| LABEL | 跑在哪 | 拦不拦合并 |
|-------|--------|------------|
| `gate` | `ci.yml` 的 `gate-tests`、`asan-smoke` | 拦（required status check） |
| `contract` | `ci.yml` 的 `contract-tests`（不在 required 列表里） | 不拦，但必须可见 |
| `slow` | `daily.yml` | 不拦 |

收紧只有一个动作：某项在 `contract` 里连续绿，就把 `test/CMakeLists.txt` 注册表里它的标签改成 `gate`。反向不成立——把一个会红的用例放进 `gate` 会立刻拦停 PR。

`ci/known-failures.txt` 是基线名单，规则是**每一项都必须当前真的失败**，否则脚本报"已经不再触发，请删掉这一行"。所以名单只能变短，把缺陷修掉的证据就是名单少一行。

`scripts/ci/check_consistency.py` 检查这几件事：命令注册表与复制写白名单是否互相自洽（注册表由 `command-table-probe` 在运行时回答，不信正则）、`test/` 下每个 `.cpp` 是否属于某个 target 或在名单里、每个 ctest target 有没有真的被某个标签选到、`conf` 里的键是否真被代码读取、端口在 conf / Dockerfile `EXPOSE` / Dockerfile `HEALTHCHECK` 三处是否一致（README 不参与这一条）。

第 7 组是**文档 ↔ 现实**：`docs/api.md` 的命令索引与总数必须等于注册表、README 的 ctest 表必须等于 `CC_TESTS`、指向 `.md` 的相对链接必须能解析、文档里`redis-cli -p <总线端口>` 这种把总线端口当客户端口的写法要判红、默认端口与 `concurrentcache_version` 必须与代码/conf 一致、文档让人拉的镜像仓库地址必须有工作流真的往那儿推；README 的命令表必须是注册表的子集（只做单向：README 一行里可以并排两个命令、CLUSTER 那一格写的是子命令，双向等值会假红，覆盖率由 api.md 那条精确等值负责）；deployment.md § 4 的表两向核对 —— 列出的键要么代码真的读（`getInt/getString/getBool`），要么行里写明「当前未被读取」，而 conf 里出现的每个键都必须在这张表里。
第 7i 组管 e2e 清单本身：`test/e2e_test/` 下的每个 `.py` 都要被 `test/e2e_test/README.md` 的清单提到、清单里点名的脚本都要真存在，文档里写的「N 个 Python 脚本 / N 个 C++ 可执行 target / N 个 ctest 用例」必须等于实际数量 —— 起因是 #99 加了一个脚本、清单却漏了 7 个，而这类数字靠手抄一定会漂。

这组判据自己也可能坏（写坏的正则会永远绿），所以 `scripts/ci/check_docs_gate_injection.py` 会往文档里逐条注入错误、要求**只有对应那一条**报 `::error::`、然后还原；它作为 `consistency` job 的一个步骤每天跟着跑。

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

> 全部 16 个用例都已注册进 CTest（`enable_testing()` 在根 `CMakeLists.txt`），每个都带 `TIMEOUT` 与 LABELS。`command-table-probe` 是工具不是用例，不注册。

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

**可执行文件**：`atomic-tests`（仅编译 `atomic_correctness_test.cpp`）

> 目录内另有 5 个带独立 `main()` 的子文件（`atomic_first/minimal/progressive/multi/memory_order_test.cpp`），**未接入构建**，需手动编译运行。

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

**可执行文件**：`cluster-tests`（单个源文件 `cluster_test/cluster_test.cpp`）

- `ClusterNode` / `ClusterState` 的槽位归属与增删
- `keyToSlot()` 落在 `[0, 16383]`
- `ClusterServer` 单例与槽表查询

> 复制/PSYNC **不在这里测**：`cluster_replication_test.cpp` 与 `cluster_strict_test.cpp` 因为要碰 `ReplicationMgr::add_to_replication_buffer()`（private）而没有接入构建，`test/CMakeLists.txt` 里写明了原因。真正的端到端复制/迁移验证在 `test/e2e_test/e2e_cluster_full_test.py`（daily 档）。

## 6. Python E2E 脚本

| 脚本 | 目的 | 时长 |
|------|------|------|
| `comparison_test.py` | **ConcurrentCache vs Redis 深度对比**（功能/性能/鲁棒性/内存） | ~5 分钟 |
| `e2e_connection_storm.py` | 并发连接洪峰（默认 2000 条，`--connections` 可调到万级） | ~1 分钟 |
| `e2e_high_concurrency_load.py` | 并发读写压测（默认 5 用户 × 20 命令，可调高） | 数十秒 |
| `e2e_consistency_check.py` | 多协程竞态一致性 | 数十秒 |
| `e2e_chaos_test.py` | 异常 / 混沌 / 恶意输入 | ~1 分钟 |
| `e2e_failover_test.py` | 主从故障转移 | ~2 分钟 |
| `e2e_cluster_full_test.py` | 集群全功能 | 数十秒 |
| `e2e_psync_replication_test.py` | PSYNC 复制验证 | 数十秒 |
| `cluster_stress_test.py` | 集群压力 | 数分钟 |
| `stress_find_limit.py` | 寻找性能极限 | 数分钟 |
| `run_all_tests.py` | 总入口，仅含连接风暴 / 高并发 / 一致性 / 混沌 4 项 | 取决于组合 |
| `test_resp_client.py` | 测试工具自身的配对断言（不连服务器，喂字节）；由 `ci.yml` 的 `consistency` job 跑 | 秒级 |

> `comparison_test.py` 需要**本机安装 redis-server 7.0.15**（测试时以 subprocess 启动），全部脚本基于原生 Python socket/asyncio 实现 RESP 客户端，无第三方 Python 依赖。failover / cluster_full / psync / cluster_stress / stress_find_limit 不在 `run_all_tests.py` 中，需单独运行。

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

> ASan 与 TSan **不能同时启用**（`CMakeLists.txt` 显式 `FATAL_ERROR`）；UBSan 可以与二者任一叠加。

| Sanitizer | 典型报告 | 排查 |
|-----------|---------|------|
| ASan | `heap-buffer-overflow` / `use-after-free` | 堆越界 / 释放后使用 |
| TSan | `data race` | 并发数据竞争 |
| UBSan | `undefined behavior` | UB：符号溢出、空指针解引用等 |

## 9. CI 集成

文件：`.github/workflows/ci.yml`

- **触发**：push / PR 到 `main` / `master`；同 `concurrency` group 内旧 run 会被取消（`main` 上的 run 不取消自己）
- **环境**：`ubuntu-24.04` + `cmake` + `build-essential` + `zlib1g-dev`
- **7 个 job**（`ci.yml`）：`build-release`（`--parallel "$(nproc)"`，上传二进制）、`build-assert`（保留 `assert` 的构建）、`gate-tests`（`ctest -L gate`）、`contract-tests`（`ctest -L contract`，非 required）、`consistency`（构建 `command-table-probe` 后跑 `scripts/ci/check_consistency.py`）、`asan-smoke`（ASan 构建 + `ctest -L "gate|contract"`）、`docker-build`（镜像冒烟）
- **required checks**：分支规则集 `main-gate` 里锁了 6 条（build-release / build-assert / gate-tests / consistency / asan-smoke / docker-build）；`contract-tests` 可见但不拦合并

> 重档（TSan/UBSan、长压测、e2e、与真 Redis 的对比、多架构编译）在 `daily.yml`，每天定时 + `workflow_dispatch` 触发，不拦 PR。

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
- [部署 § 2.4 Sanitizer 选项](deployment.md)
- [API 文档](api.md) — 命令级测试输入
