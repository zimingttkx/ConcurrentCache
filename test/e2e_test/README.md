# E2E End-to-End Tests for ConcurrentCache

本目录包含针对 ConcurrentCache 的端到端自动化压测和正确性验证脚本。

## 测试脚本列表

| 脚本 | 功能描述 |
|------|---------|
| `e2e_connection_storm.py` | 海量连接洪峰测试 - 瞬间发起 10000+ 并发 TCP 连接 |
| `e2e_high_concurrency_load.py` | 超高并发读写压测 - 模拟 1000 虚拟用户持续压测 |
| `e2e_consistency_check.py` | 并发正确性与竞态校验 - 多协程并发修改同一 Key |
| `e2e_chaos_test.py` | 异常与混沌测试 - TCP Reset、巨大 Payload、畸形协议 |
| `run_all_tests.py` | 运行全部测试并生成汇总报告 |

## 快速开始

### 前置条件

- Python 3.7+
- ConcurrentCache 服务端运行在 `127.0.0.1:6379`

### 运行单个测试

```bash
# 海量连接洪峰测试
python3 e2e_connection_storm.py --host 127.0.0.1 --port 6379

# 超高并发读写压测
python3 e2e_high_concurrency_load.py --host 127.0.0.1 --port 6379 --users 1000

# 并发正确性校验
python3 e2e_consistency_check.py --host 127.0.0.1 --port 6379

# 混沌测试
python3 e2e_chaos_test.py --host 127.0.0.1 --port 6379
```

### 运行全部测试

```bash
python3 run_all_tests.py
```

## 测试详情

### 1. Connection Storm Test (`e2e_connection_storm.py`)

**目标**：验证极限连接数下服务端不崩溃

**参数**：
- `--connections`: 目标连接数（默认 10000）
- `--batch-size`: 每批连接数（默认 500）

**通过标准**：
- 成功建立 10000+ 连接
- 服务端未崩溃
- 90%+ 连接 PING 响应成功

### 2. High Concurrency Load Test (`e2e_high_concurrency_load.py`)

**目标**：模拟真实用户负载，输出 QPS/延迟统计

**参数**：
- `--users`: 虚拟用户数（默认 1000）
- `--commands`: 每用户命令数（默认 100）

**命令配比**：70% GET / 20% SET / 10% DEL

**通过标准**：
- QPS > 1000
- P99 延迟 < 500ms
- 错误率 < 5%

### 3. Data Consistency Check (`e2e_consistency_check.py`)

**目标**：验证并发修改同一 Key 的一致性

**测试场景**：
- 场景 A：10 协程 x 100 次读-增-写 = 预期结果 1000（服务端约 60 连接/秒，测试需约 30 秒）
- 场景 B：100 协程并发覆盖写同一 Key
- 场景 C：读写并发，一边写一边读

**通过标准**：
- 场景 A：最终值严格等于 10000
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
6. 阻塞命令（BLPOP）

**通过标准**：所有测试后服务端仍存活

## 输出格式

所有测试输出 JSON 格式日志：

```json
{
  "test_name": "connection_storm",
  "timestamp": "2026-05-13T22:00:00",
  "result": "PASS",
  "metrics": {
    "total_connections": 10500,
    "successful_connections": 10480,
    "failed_connections": 20,
    "success_rate": 0.998
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

1. **连接数限制**：Linux 默认文件描述符限制为 1024，运行大规模连接测试前需执行：
   ```bash
   ulimit -n 65535
   ```

2. **端口范围**：大量连接测试可能耗尽本地临时端口，需确保端口范围足够大。

3. **测试顺序**：建议按以下顺序运行：
   1. `e2e_connection_storm.py` - 验证基础连接能力
   2. `e2e_high_concurrency_load.py` - 验证负载能力
   3. `e2e_consistency_check.py` - 验证数据一致性
   4. `e2e_chaos_test.py` - 验证容错能力

4. **超时设置**：复杂测试可能需要较长时间，可在脚本内调整 timeout 参数。