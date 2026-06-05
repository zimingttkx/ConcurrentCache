# ConcurrentCache 测试文档

> **文档版本**：v3.2 · 适用服务端版本 V3.0+
> **测试体系**：C++ 单元/集成测试 + Python 端到端压测 + GitHub Actions CI
> **入口**：`test/CMakeLists.txt` 定义全部 C++ 测试目标；`test/e2e_test/run_all_tests.py` 驱动全部 E2E 脚本

---

## 📑 目录

- [1. 概览](#1-概览)
- [2. 测试体系结构](#2-测试体系结构)
- [3. 快速运行](#3-快速运行)
- [4. 断言与测试工具](#4-断言与测试工具)
- [5. C++ 测试套件](#5-c-测试套件)
  - 5.1 [原子操作测试](#51-原子操作测试)
  - 5.2 [锁机制测试](#52-锁机制测试)
  - 5.3 [同步原语测试](#53-同步原语测试)
  - 5.4 [存储层测试](#54-存储层测试)
  - 5.5 [数据类型测试](#55-数据类型测试)
  - 5.6 [RDB 持久化测试](#56-rdb-持久化测试)
  - 5.7 [命令层测试](#57-命令层测试)
  - 5.8 [集群测试](#58-集群测试)
  - 5.9 [网络压测](#59-网络压测)
  - 5.10 [性能压测](#510-性能压测)
- [6. E2E 端到端测试（Python）](#6-e2e-端到端测试python)
- [7. CI 持续集成](#7-ci-持续集成)
- [8. 测试覆盖与基线](#8-测试覆盖与基线)
- [9. 故障排查](#9-故障排查)
- [10. 另见](#10-另见)

---

## 1. 概览

ConcurrentCache 测试体系覆盖三大层次：

| 层次 | 工具 | 范围 |
|------|------|------|
| **单元测试** | C++ `test_assertions.h` 断言宏 + `ctest` | 单一组件正确性（原子、锁、对象、容器） |
| **集成测试** | C++ + `ctest` | 模块间协作（命令层 ↔ 存储层、存储层 ↔ ExpireDict） |
| **端到端压测** | Python 脚本 + `redis-py` | 真实场景下并发/容错/混沌 |

### 1.1 测试目标

| 目标 | 指标 |
|------|------|
| 功能正确性 | 全部单元 + 集成测试 PASS |
| 并发安全 | 死锁/竞态/数据竞争 0 容忍 |
| 性能基线 | GET/SET ≥ 10 万 QPS（单实例、64 分片） |
| 故障容错 | 异常输入/网络抖动下服务不崩溃 |

---

## 2. 测试体系结构

### 2.1 目录布局

```
test/
├── CMakeLists.txt                  # C++ 测试构建入口
├── test_main.cpp                   # V1/V2 旧版测试入口
├── test_v3_main.cpp                # V3 测试入口（当前）
├── tests.cpp                       # 旧版测试合集
├── trace/
│   ├── test_assertions.h           # 断言宏与 TestSuite 类
│   ├── trace_logger.h              # 测试日志追踪
│   └── trace_analyzer.h            # 测试结果分析
│
├── atomic_test/                    # 原子操作测试
├── lock_test/                      # 锁机制测试
├── sync_primitives_test/           # 同步原语（CountDownLatch 等）
├── storage_test/                   # 存储层
├── datatype_test/                  # 数据类型（CacheObject）
├── persistence_test/               # RDB 持久化
├── command_test/                   # 命令层
├── cluster_test/                   # 集群
├── network_test/                   # 网络压测
├── stress_test/                    # 性能压测
└── e2e_test/                       # E2E 压测（Python）
    ├── README.md
    ├── run_all_tests.py
    ├── e2e_connection_storm.py
    ├── e2e_high_concurrency_load.py
    ├── e2e_consistency_check.py
    ├── e2e_chaos_test.py
    ├── e2e_failover_test.py
    ├── e2e_cluster_full_test.py
    ├── e2e_psync_replication_test.py
    └── stress_find_limit.py
```

### 2.2 测试目标清单

| 可执行文件 | 测试模块 | 入口 |
|-----------|---------|------|
| `atomic-tests` | 原子操作 | `atomic_test/atomic_correctness_test.cpp` |
| `lock-correctness-tests` | 锁正确性 | `lock_test/lock_correctness_test.cpp` |
| `lock-deadlock-tests` | 死锁检测 | `lock_test/lock_deadlock_test.cpp` |
| `lock-race-tests` | 竞态检测 | `lock_test/lock_race_test.cpp` |
| `lock-boundary-tests` | 锁边界 | `lock_test/lock_boundary_test.cpp` |
| `sync-primitives-tests` | 同步原语 | `sync_primitives_test/sync_primitives_test.cpp` |
| `concurrentcache-v3-tests` | V3 综合 | `test_v3_main.cpp`（含 datatype/storage/rdb） |
| `stress-test` | 性能压测 | `stress_test/stress_test.cpp` |
| `long-running-stress-test` | 长期压测 | `stress_test/long_running_stress_test.cpp` |
| `load-limit-test` | 极限负载 | `stress_test/load_limit_test.cpp` |
| `network-stress-test` | 网络压测 | `network_test/network_stress_test.cpp` |
| `cluster-tests` | 集群 | `cluster_test/cluster_test.cpp` |

---

## 3. 快速运行

### 3.1 编译全部测试

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

### 3.2 运行全部 C++ 测试

```bash
ctest --test-dir build --output-on-failure
```

### 3.3 运行单个测试可执行文件

```bash
# V3 综合测试
./build/test/concurrentcache-v3-tests

# 仅运行 storage 部分
./build/test/concurrentcache-v3-tests --storage

# 仅 datatype / rdb
./build/test/concurrentcache-v3-tests --datatype
./build/test/concurrentcache-v3-tests --rdb
```

### 3.4 运行 Python E2E 测试

```bash
# 先启动服务
./build/concurrentcache-server &

# 单项测试
python3 test/e2e_test/e2e_connection_storm.py
python3 test/e2e_test/e2e_high_concurrency_load.py --users 1000
python3 test/e2e_test/e2e_consistency_check.py
python3 test/e2e_test/e2e_chaos_test.py

# 一键跑全部
python3 test/e2e_test/run_all_tests.py
```

> **运行前调整**：
> ```bash
> ulimit -n 65535   # 提升文件描述符上限，连接洪峰测试必需
> ```

### 3.5 启用 Sanitizer

```bash
# AddressSanitizer（内存越界/UAF）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON

# ThreadSanitizer（数据竞争）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON

# UndefinedBehaviorSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
```

> 三种 Sanitizer **不可同时启用**，编译时需挑选一种。

---

## 4. 断言与测试工具

### 4.1 断言宏

`test/trace/test_assertions.h` 提供完整测试框架：

| 宏 | 用途 | 失败时 |
|---|------|--------|
| `EXPECT_TRUE(cond)` | 断言为真 | 继续执行 |
| `EXPECT_FALSE(cond)` | 断言为假 | 继续执行 |
| `EXPECT_EQ(a, b)` | 断言相等 | 继续执行 |
| `EXPECT_NE(a, b)` | 断言不等 | 继续执行 |
| `EXPECT_LT/LE/GT/GE` | 数值比较 | 继续执行 |
| `EXPECT_TRUE_TIMEOUT(cond, ms)` | 限时断言 | 继续执行 |
| `ASSERT_TRUE/FALSE/EQ/NE` | 强断言 | **立即终止当前测试** |
| `SKIP()` | 跳过当前测试 | 计入 skipped |

### 4.2 测试套件

使用 `TEST_SUITE("name")` 宏创建套件，自动输出统计：

```
========================================
  Test Suite: GlobalStorage Basic Operations
========================================
...
----------------------------------------
Results for: GlobalStorage Basic Operations
  Total:  8
  Passed: 8
  Failed: 0
  Skipped: 0
----------------------------------------
```

### 4.3 超时工具

`run_with_timeout(func, ms)` — 限时执行函数，超时返回 false。用于网络/阻塞调用测试。

### 4.4 全局统计

`g_test_stats()` 单例统计 PASS/FAIL/SKIP 数量，所有断言自动累加。

---

## 5. C++ 测试套件

---

### 5.1 原子操作测试

**目标**：验证 `std::atomic` 各内存序下的正确性。

**可执行文件**：`atomic-tests`
**源文件**：`atomic_test/atomic_correctness_test.cpp`（含 6 个子文件）

| 子文件 | 测试内容 |
|--------|---------|
| `atomic_first_test.cpp` | 基础 fetch_add / compare_exchange |
| `atomic_minimal_test.cpp` | 最小可重现样例 |
| `atomic_progressive_test.cpp` | 渐进式复杂场景 |
| `atomic_multi_test.cpp` | 多线程竞争场景 |
| `atomic_memory_order_test.cpp` | memory_order_relaxed/acquire/release 语义 |

**运行**：

```bash
./build/test/atomic-tests
```

**通过标准**：所有用例 PASS。

---

### 5.2 锁机制测试

**目标**：验证 Mutex、SpinLock、RWLock、ShardedLock 在并发场景下的正确性。

**可执行文件**（4 个独立）：

| 可执行文件 | 测试重点 |
|-----------|---------|
| `lock-correctness-tests` | 基本加锁/解锁、互斥语义 |
| `lock-deadlock-tests` | 死锁检测、嵌套加锁顺序、try_lock 行为 |
| `lock-race-tests` | 高并发竞态下的数据一致性 |
| `lock-boundary-tests` | 边界条件（空容器、极端并发数、长时间持锁） |

**额外文件**：`lock_test/lock_stress_test.cpp` — 长期持锁/重负载稳定性。

**运行**：

```bash
./build/test/lock-correctness-tests
./build/test/lock-deadlock-tests
./build/test/lock-race-tests
./build/test/lock-boundary-tests
```

**通过标准**：

- `lock-correctness` 全部 PASS
- `lock-deadlock` 死锁场景下正确报错或回退，无进程挂起
- `lock-race` 100 万次并发操作后数据完全一致
- `lock-boundary` 无死锁、无 panic

---

### 5.3 同步原语测试

**目标**：验证 CountDownLatch、CyclicBarrier、Semaphore 等原语。

**可执行文件**：`sync-primitives-tests`
**源文件**：`sync_primitives_test/sync_primitives_test.cpp`

**测试内容**：

- CountDownLatch 计数准确性
- CyclicBarrier 多轮同步
- Semaphore 资源池限流

**运行**：

```bash
./build/test/sync-primitives-tests
```

---

### 5.4 存储层测试

**目标**：验证 `GlobalStorage` 64 分片存储的增删改查、TTL、并发一致性。

**可执行文件**：`concurrentcache-v3-tests`（含子套件 `--storage`）
**源文件**：`storage_test/storage_v3_test.cpp`

| 测试套件 | 覆盖点 |
|---------|--------|
| `GlobalStorage Basic Operations` | SET / GET / DEL / EXISTS / SIZE |
| `GlobalStorage TTL` | 相对过期、惰性删除、过期后不可读 |
| `GlobalStorage Set Expire Time` | 绝对时间戳设置 |
| `GlobalStorage Set With Expire` | 原子设置值与过期 |
| `GlobalStorage Persist` | 移除过期时间 |
| `GlobalStorage Dirty Counter` | 脏计数（用于 RDB 触发判断） |
| `GlobalStorage Concurrent Read Write` | 8 线程 × 1000 操作并发一致性 |
| `GlobalStorage Sharding Performance` | 8 线程插入 10000 key 性能 |
| `GlobalStorage Large Dataset` | 10 万 key 插入与随机读 |
| `GlobalStorage Multiple DataTypes` | 5 种类型混合存储 |
| `GlobalStorage Get All Objects` | 全量导出（RDB 持久化前置步骤） |

**运行**：

```bash
./build/test/concurrentcache-v3-tests --storage
```

**通过标准**：

- 所有断言 PASS
- 8 线程并发读写的最终 `size()` 等于预期
- 10000 key 插入 < 2 秒（取决于硬件）

---

### 5.5 数据类型测试

**目标**：验证 `CacheObject` 对 String/List/Hash/Set/ZSet 五种类型的支持。

**可执行文件**：`concurrentcache-v3-tests`（子套件 `--datatype`）
**源文件**：`datatype_test/object_test.cpp`

**测试内容**：

- 字符串设置/获取
- List push/pop/range/len
- Hash set/get/del/len/items
- Set add/contains/size/members/remove
- ZSet add/score/card/range/remove

**运行**：

```bash
./build/test/concurrentcache-v3-tests --datatype
```

---

### 5.6 RDB 持久化测试

**目标**：验证 RDB 文件写入、读取、恢复的正确性。

**可执行文件**：`concurrentcache-v3-tests`（子套件 `--rdb`）
**源文件**：`persistence_test/rdb_test.cpp`

**测试内容**：

- 写入 RDB 文件
- 读取并还原到 `GlobalStorage`
- 不同数据类型的序列化/反序列化
- 大数据集（10 万 key）的快照与恢复
- 空数据库、空 value、特殊字符

**运行**：

```bash
./build/test/concurrentcache-v3-tests --rdb
```

---

### 5.7 命令层测试

**目标**：验证 `CommandFactory` 注册的全部命令返回值符合 RESP 协议。

**可执行文件**：（当前版本在 `test_v3_main.cpp` 中**暂时禁用**，源码已就绪）
**源文件**：`command_test/command_test.cpp`

> 该测试模块源码完整，但 `test_v3_main.cpp` 注释了 `run_all_command_tests()` 的调用。启用方法：取消第 70-72 行的注释，重新编译。

**测试套件**：

| 套件 | 覆盖命令 |
|------|---------|
| `String Commands` | SET / GET / DEL / EXISTS |
| `List Commands` | LPUSH / RPUSH / LPOP / RPOP / LLEN / LRANGE |
| `Hash Commands` | HSET / HGET / HDEL / HLEN / HGETALL |
| `Set Commands` | SADD / SISMEMBER / SCARD / SREM / SMEMBERS |
| `ZSet Commands` | ZADD / ZSCORE / ZCARD / ZREM / ZRANGE |
| `TTL Commands` | EXPIRE / TTL / PERSIST / SETEX |
| `General Commands` | PING / DBSIZE / FLUSHDB |
| `RDB Commands` | SAVE / BGSAVE / LASTSAVE |
| `Invalid Commands` | 不存在命令 / 参数不足 |

**每个套件的断言**：返回的 RESP 字符串与预期完全匹配（如 `+OK\r\n`、`$6\r\nvalue\r\n`）。

---

### 5.8 集群测试

**目标**：验证集群节点间通信、槽位分配、主从握手。

**可执行文件**：`cluster-tests`
**源文件**：`cluster_test/cluster_test.cpp`（含 `cluster_replication_test.cpp`、`cluster_strict_test.cpp`）

**测试内容**：

- 节点 Gossip 心跳
- 哈希槽分配
- PSYNC 握手
- 复制偏移量
- 故障检测

**运行**：

```bash
./build/test/cluster-tests
```

---

### 5.9 网络压测

**目标**：模拟高并发 TCP 连接，验证 SubReactor 线程池稳定性。

**可执行文件**：`network-stress-test`
**源文件**：`network_test/network_stress_test.cpp` + `network_stress_test.h` + `network_stress_test_main.cpp`

**测试场景**：

- 数万并发连接建立/关闭
- 短连接 vs 长连接混合
- 慢客户端（slow consumer）场景

**运行**：

```bash
./build/test/network-stress-test
```

---

### 5.10 性能压测

**目标**：极限性能基线，识别瓶颈。

**可执行文件**（3 个独立）：

| 可执行文件 | 测试重点 | 时长 |
|-----------|---------|------|
| `stress-test` | 高并发读写混合压力 | 数十秒 |
| `long-running-stress-test` | 长时间（数小时）稳定性 | 数小时 |
| `load-limit-test` | 逐步加大负载找到性能拐点 | 数分钟 |

**源文件**：

- `stress_test/stress_test.cpp`
- `stress_test/long_running_stress_test.cpp`
- `stress_test/load_limit_test.cpp`

**运行**：

```bash
./build/test/stress-test                    # 短期压力
./build/test/load-limit-test                # 找极限
./build/test/long-running-stress-test &     # 长期后台跑
```

**通过标准**：

- QPS ≥ 100,000（单实例、8 核、64 分片）
- P99 延迟 ≤ 5ms
- 错误率 ≤ 0.1%
- 长时间无内存泄漏（用 ASan 验证）

---

## 6. E2E 端到端测试（Python）

> 全部脚本位于 `test/e2e_test/`，基于 Python 3.7+ 与 `redis-py`。

### 6.1 脚本清单

| 脚本 | 目的 | 时长 |
|------|------|------|
| `e2e_connection_storm.py` | 10000+ 并发连接洪峰 | ~1 分钟 |
| `e2e_high_concurrency_load.py` | 1000 虚拟用户压测 | 数十秒 |
| `e2e_consistency_check.py` | 多协程竞态一致性 | 数十秒 |
| `e2e_chaos_test.py` | 异常/混沌/恶意输入 | ~1 分钟 |
| `e2e_failover_test.py` | 主从故障转移 | ~2 分钟 |
| `e2e_cluster_full_test.py` | 集群全功能 | 数十秒 |
| `e2e_psync_replication_test.py` | PSYNC 复制验证 | 数十秒 |
| `stress_find_limit.py` | 寻找性能极限 | 数分钟 |
| `run_all_tests.py` | 上述测试总入口 | 取决于组合 |

### 6.2 Connection Storm（连接洪峰）

**目标**：极限连接数下服务端不崩溃。

```bash
python3 e2e_connection_storm.py --host 127.0.0.1 --port 6379 \
  --connections 10000 --batch-size 500
```

**通过标准**：

- 成功建立 10000+ 连接
- 服务端未崩溃
- 90%+ 连接 PING 响应成功

### 6.3 High Concurrency Load（高并发负载）

```bash
python3 e2e_high_concurrency_load.py --host 127.0.0.1 --port 6379 \
  --users 1000 --commands 100
```

**命令配比**：70% GET / 20% SET / 10% DEL

**通过标准**：

- QPS > 1000
- P99 延迟 < 500ms
- 错误率 < 5%

### 6.4 Data Consistency（一致性校验）

**测试场景**：

| 场景 | 期望 |
|------|------|
| 10 协程 × 100 次 INCR 同一 key | 最终值严格 = 1000 |
| 100 协程并发覆盖写同一 key | 最终值是某个有效写入值 |
| 读写并发混合 | 无异常值读取 |

### 6.5 Chaos Test（混沌测试）

**测试用例**：

1. TCP Half-Close（RST 断开）
2. 巨大 Payload（50MB）
3. 畸形 RESP 协议
4. 快速连接断开（1000 连接/秒）
5. 并发异常请求
6. 阻塞命令（BLPOP）

**通过标准**：所有用例后服务端仍存活。

### 6.6 Failover Test（故障转移）

验证主从切换流程：

- 主节点 kill
- 从节点检测超时
- 从节点晋升
- 客户端重连到新主

### 6.7 PSYNC Replication

验证主从增量复制：

- 主写入若干 key
- 从 PSYNC 握手
- 增量同步偏移量比对
- 数据最终一致

### 6.8 一键运行

```bash
python3 run_all_tests.py
```

**输出**：

- 控制台实时日志
- 汇总报告 `e2e_report.json`：

```json
{
  "timestamp": "2026-06-05T10:30:00",
  "total_tests": 4,
  "passed_tests": 4,
  "failed_tests": 0,
  "results": [
    {"name": "Connection Storm Test", "passed": true, "metrics": {...}},
    ...
  ]
}
```

**退出码**：0 全部通过 / 1 有失败。

### 6.9 注意事项

| 注意点 | 说明 |
|--------|------|
| **文件描述符** | `ulimit -n 65535`（默认 1024 远不够） |
| **端口范围** | 大量短连接会耗尽本地临时端口 |
| **测试顺序** | 推荐：storm → load → consistency → chaos |
| **超时** | 单脚本 5 分钟超时（在 `run_all_tests.py` 中可调） |

---

## 7. CI 持续集成

### 7.1 GitHub Actions 工作流

文件：`.github/workflows/ci.yml`

**触发条件**：

- push 到 `main` / `master` 分支
- pull request 到 `main` / `master`

**执行步骤**：

1. 拉取代码
2. 安装依赖：`cmake`、`build-essential`、`zlib1g-dev`
3. Configure：`cmake .. -DCMAKE_BUILD_TYPE=Release`
4. Build：`cmake --build build --parallel 2`
5. 上传产物：`concurrentcache-server` 二进制（保留 1 天）

**运行环境**：`ubuntu-24.04`

### 7.2 本地模拟 CI

```bash
# 镜像 CI 流程
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel 2
ls -la concurrentcache-server
```

---

## 8. 测试覆盖与基线

### 8.1 当前覆盖范围

| 模块 | 覆盖情况 | 测试入口 |
|------|---------|---------|
| 原子操作 | ✅ 高 | `atomic-tests` |
| 锁机制 | ✅ 高 | `lock-*-tests`（4 个） |
| 同步原语 | ✅ 高 | `sync-primitives-tests` |
| 存储层 | ✅ 高 | `concurrentcache-v3-tests --storage` |
| 数据类型 | ✅ 高 | `concurrentcache-v3-tests --datatype` |
| RDB 持久化 | ✅ 高 | `concurrentcache-v3-tests --rdb` |
| 命令层 | ⚠️ 源码就绪，未启用 | （手动启用） |
| 网络层 | ✅ 中 | `network-stress-test` |
| 集群 | ✅ 中 | `cluster-tests` |
| 性能压测 | ✅ 中 | `stress-test` 等 |
| E2E 混沌 | ✅ 高 | `e2e_chaos_test.py` |

### 8.2 性能基线（参考值）

> 以下数据基于 8 核 CPU、Release 构建、单实例、64 分片：

| 场景 | 目标 QPS | 目标 P99 |
|------|---------|---------|
| GET 单 key | ≥ 200,000 | ≤ 1ms |
| SET 单 key | ≥ 150,000 | ≤ 2ms |
| 混合 7:2:1 (G:S:D) | ≥ 100,000 | ≤ 5ms |
| HSET 100 fields | ≥ 50,000 | ≤ 10ms |
| LRANGE 100 元素 | ≥ 80,000 | ≤ 3ms |
| 10000 并发连接 | 服务稳定 | — |

---

## 9. 故障排查

### 9.1 测试编译失败

| 症状 | 排查 |
|------|------|
| `undefined reference to cc_server::*` | 检查 `test/CMakeLists.txt` 的 `COMMON_SOURCES` 是否包含新增的源文件 |
| `zlib not found` | 安装 `zlib1g-dev` |
| `C++20 features not supported` | 升级 GCC ≥ 12 或 Clang ≥ 16 |

### 9.2 死锁/挂起

```bash
# 用 gdb attach 到挂起的进程
gdb -p <pid>
(gdb) thread apply all bt
```

`lock-deadlock-tests` 设计上应能检测并报告死锁场景。

### 9.3 Sanitizer 报告

| Sanitizer | 典型输出 | 含义 |
|-----------|---------|------|
| ASan | `heap-buffer-overflow` | 堆越界 |
| ASan | `use-after-free` | 释放后使用 |
| TSan | `data race` | 数据竞争 |
| UBSan | `undefined behavior` | 未定义行为 |

发现报告后定位到具体行号修复源码，**不要在测试中屏蔽 sanitizer**。

### 9.4 E2E Python 报错

| 症状 | 排查 |
|------|------|
| `ConnectionRefusedError` | 服务端未启动或端口不对 |
| `ulimit: too many open files` | 执行 `ulimit -n 65535` |
| 大量 timeout | 服务端压力大，减小 `--users` / `--commands` |

---

## 10. 另见

### 10.1 相关文档

- [项目 README](../../README.md) — 编译、运行、测试快速上手
- [架构总览](../developing/Architecture.md) — 系统架构演进
- [API 参考](./API.md) — 命令协议层
- [部署手册](./DEPLOY.md) — 部署与运维

### 10.2 测试源码索引

- 断言宏：`test/trace/test_assertions.h`
- V3 入口：`test/test_v3_main.cpp`
- E2E 入口：`test/e2e_test/run_all_tests.py`
- 测试构建：`test/CMakeLists.txt`

### 10.3 扩展阅读

- [GoogleTest 文档](https://google.github.io/googletest/) — C++ 主流测试框架（当前项目自研轻量框架）
- [pytest 文档](https://docs.pytest.org/) — Python 测试参考
- [AddressSanitizer 文档](https://github.com/google/sanitizers/wiki/AddressSanitizer)
