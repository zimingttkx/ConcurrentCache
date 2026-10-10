# E2E End-to-End Tests for ConcurrentCache

本目录包含针对 ConcurrentCache 的端到端自动化压测和正确性验证脚本。

## 测试脚本列表

本目录共 12 个 Python 脚本，按"要不要自己起服务器"分三类：

| 脚本 | 功能描述 | 服务器 |
|------|---------|--------|
| `e2e_connection_storm.py` | 海量连接洪峰 - 默认 2000 条并发 TCP 连接（`--connections` 可调到万级） | 需外部已启动 |
| `e2e_high_concurrency_load.py` | 并发读写压测 - 默认 5 用户 × 20 命令，`--users`/`--commands` 可调高 | 需外部已启动 |
| `e2e_consistency_check.py` | 并发正确性与竞态校验 - 多协程并发修改同一 Key | 需外部已启动 |
| `e2e_chaos_test.py` | 异常与混沌测试 - TCP Reset、50MB 巨大 Payload、畸形 RESP | 需外部已启动 |
| `run_all_tests.py` | 本地一键入口，**只含上面 4 项**（不是全部脚本） | 需外部已启动 |
| `stress_find_limit.py` | 逐步加压找性能拐点，产出 `stress_limit_report.json` | 自己拉起（默认端口 16379） |
| `cluster_stress_test.py` | 三节点集群压力（16379/16380/16381） | 自己拉起 |
| `e2e_cluster_full_test.py` | 集群全功能：MEET / 槽位 / 复制 / `CLUSTER MIGRATE` / failover 检测 / 优雅退出 | 自己拉起（三节点） |
| `e2e_failover_test.py` | 主节点失联后的故障转移验证 | 自己拉起 |
| `e2e_psync_replication_test.py` | 内部复制链路验证（`CLUSTER REPLICATE` + 总线 `REPLSYNC`，**不是** Redis 的 PSYNC 握手 —— 那三条命令已被显式拒绝，见 `docs/api.md` §13） | 自己拉起 |
| `comparison_test.py` | 与真 Redis 的逐项对比，产出 `comparison_report.json` | 自己拉起两边；**需要本机安装 `redis-server`** |
| `test_resp_client.py` | 测试工具自身的配对断言（喂字节，不连服务器）；由 `ci.yml` 的 `consistency` job 执行 | 不需要 |

> 哪些脚本真的被 CI 跑：`daily.yml` 跑 failover / psync / cluster_stress / cluster_full /
> stress_find_limit / comparison；connection storm、high concurrency、consistency、chaos 四个
> 目前只在本地经 `run_all_tests.py` 跑（未接进任何 job，登记在 `ci/known-failures.txt` 的
> `legacy-file` 里）。`test_resp_client.py` 接的是 `ci.yml` 的 `consistency`。

## 快速开始

### 前置条件

- Python 3.7+，无第三方依赖（原生 `socket` / `asyncio` 实现 RESP 客户端）
- 前四个脚本需要一个已启动的服务端，默认打在 `127.0.0.1:6379`（各脚本都有 `--port` 可改）
- `comparison_test.py` 额外需要本机 `redis-server`
- 大规模连接测试前先放开文件描述符：`ulimit -n 65535`

### 运行单个测试

```bash
# 需要外部已启动服务端的那几个：先起服务器，再指定端口
./build/concurrentcache-server &
python3 e2e_connection_storm.py --host 127.0.0.1 --port 6379
python3 e2e_high_concurrency_load.py --host 127.0.0.1 --port 6379 --users 1000 --commands 200
python3 e2e_consistency_check.py --host 127.0.0.1 --port 6379
python3 e2e_chaos_test.py --host 127.0.0.1 --port 6379

# 自己拉起服务器的：直接跑，不用先起进程
python3 e2e_cluster_full_test.py
python3 stress_find_limit.py
python3 comparison_test.py

# 测试工具自检（不连服务器）
python3 test_resp_client.py
```

### 运行"全部"测试

```bash
python3 run_all_tests.py
```

> `run_all_tests.py` 只串起连接风暴 / 高并发 / 一致性 / 混沌这 4 项，且都要求服务端已经在跑；
> 集群与对比类脚本要单独执行（见上表）。

## 测试详情

### 1. Connection Storm Test (`e2e_connection_storm.py`)

**目标**：验证极限连接数下服务端不崩溃

**参数**：
- `--connections`: 目标连接数（默认 2000）
- `--batch-size`: 每批连接数（默认 200）
- `--port`: 服务端端口（默认 6379）

**通过标准**：
- 服务端未崩溃
- PING 成功率 > 70%

### 2. High Concurrency Load Test (`e2e_high_concurrency_load.py`)

**目标**：模拟真实用户负载，输出 QPS/延迟统计

**参数**：
- `--users`: 虚拟用户数（默认 5）
- `--commands`: 每用户命令数（默认 20）
- `--port`: 服务端端口（默认 6379）

**命令配比**：70% GET / 20% SET / 10% DEL

**通过标准**（脚本自身的判据）：
- 错误率 ≤ 15% 且 QPS > 0
- P99 等延迟只统计输出，不参与判定

### 3. Data Consistency Check (`e2e_consistency_check.py`)

**目标**：验证并发修改同一 Key 的一致性

**测试场景**：
- 场景 A：10 协程 x 100 次读-增-写 = 预期结果 1000（服务端约 60 连接/秒，测试需约 30 秒）
- 场景 B：100 协程并发覆盖写同一 Key
- 场景 C：读写并发，一边写一边读

**通过标准**：
- 场景 A：10 协程 × 100 次读-改-写，理论上限 1000；最终值 ≤ 1000 即通过（小于 1000 表示竞态丢更新，属预期），超过 1000 才判失败
- 场景 B：最终值是某个有效写入值
- 场景 C：无异常值读取

### 4. Chaos Test (`e2e_chaos_test.py`)

**目标**：验证服务端对恶意输入的容错能力

**测试用例**：
1. TCP Half-Close（RST 断开）
2. 巨大 Payload（50MB）
3. 畸形 RESP 协议
4. 快速连接断开（1000 连接/秒）
5. 并发异常请求
6. 不支持的阻塞命令（脚本故意发 `BLPOP`：本服务器没有阻塞命令，这是在断言它被拒绝而不是把连接挂住）

**通过标准**：所有测试后服务端仍存活

## 输出格式

前四个脚本（连接风暴 / 高并发 / 一致性 / 混沌）会打印 JSON 结构的日志；`stress_find_limit.py`、`comparison_test.py`、`e2e_psync_replication_test.py` 各写自己的 `*.json` 报告文件；`test_resp_client.py` 只输出文本 PASS/FAIL 并以退出码判定。

以连接风暴为例（默认 `--connections 2000`）：

```json
{
  "test_name": "connection_storm",
  "timestamp": "2026-05-13T22:00:00",
  "result": "PASS",
  "metrics": {
    "total_connections": 2000,
    "successful_connections": 1980,
    "failed_connections": 20,
    "success_rate": 0.99
  }
}
```

## 报告输出

运行 `run_all_tests.py` 后会生成 `e2e_report.json` 汇总报告：

```json
{
  "timestamp": "2026-05-13T22:00:00",
  "total_tests": 4,
  "passed_tests": 4,
  "failed_tests": 0,
  "results": [...]
}
```

## 注意事项

1. **端口范围**：大量连接测试可能耗尽本地临时端口，需确保端口范围足够大。

2. **测试顺序**：本地手工回归建议按 `e2e_connection_storm.py` → `e2e_high_concurrency_load.py` → `e2e_consistency_check.py` → `e2e_chaos_test.py` 的顺序跑（也就是 `run_all_tests.py` 串的这 4 项），之后再单独跑集群/对比类脚本。

3. **超时设置**：复杂测试可能需要较长时间，可在脚本内调整 timeout 参数。