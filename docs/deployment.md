# 部署与运维

> **目标平台**：Linux x86_64（推荐 Ubuntu 24.04 / Debian 12）
> **部署方式**：源码编译 / Docker / Docker Compose
> **默认端口**：`6379`（客户端）+ `16379`（集群总线，`port + 10000`）
> **前置依赖**：仅 ZLIB（系统库）

## 1. 编译选项

`CMakeLists.txt` 提供的 options：

| Option | 默认 | 说明 |
|--------|------|------|
| `CMAKE_BUILD_TYPE` | （未设） | `Release` / `Debug` |
| `ENABLE_ASAN` | `OFF` | AddressSanitizer（内存越界 / UAF） |
| `ENABLE_TSAN` | `OFF` | ThreadSanitizer（数据竞争） |
| `ENABLE_UBSAN` | `OFF` | UndefinedBehaviorSanitizer |
| `ENABLE_TSAN AND ENABLE_ASAN` | — | **不可同时启用**，会 FATAL_ERROR |

**编译标志**（GCC/Clang）：

```text
-Wall -Wextra -Werror
-Wconversion -Wshadow -Wsign-conversion -Wdouble-promotion
```

> 上面那组是**服务端 target** 的标志（`CMakeLists.txt:120-122`）。测试 target 用同一组 `-W`，
> 但**不带 `-Werror`**（`test/CMakeLists.txt:212-216`）——那些文件里留着历史警告，把它们一起
> 变成致命错误会把"清理测试"混进日常门禁。所以"Release 构建全部 target 都过 -Werror"这个说法
> 不成立，`docs/CONTRIBUTING.md` 的门禁表里也按"服务端 target"来写。

## 2. 源码编译

### 2.1 系统要求

| 项 | 版本 |
|----|------|
| GCC | ≥ 12 |
| Clang | ≥ 16 |
| CMake | ≥ 3.20 |
| ZLIB | 任意 |
| Linux Kernel | ≥ 3.10（`epoll_wait`） |
| 体系结构 | x86_64（项目用 `<sys/epoll.h>` 等 POSIX API） |

> **Windows 不可直接运行**——源码使用 POSIX API（`sys/epoll.h`、`unistd.h`、`fcntl.h`）。CI / Docker / 集群部署都在 Linux。

### 2.2 安装依赖

```bash
# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y cmake build-essential zlib1g-dev

# CentOS / RHEL
sudo yum install -y cmake gcc-c++ zlib-devel
```

### 2.3 编译

```bash
git clone https://github.com/zimingttkx/ConcurrentCache.git
cd ConcurrentCache
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

**产物**：`build/concurrentcache-server`（约 4MB Release 二进制）

### 2.4 启用 Sanitizer

```bash
# ASan（推荐先跑一遍）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build

# TSan（重点查数据竞争）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build

# UBSan
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
cmake --build build
```

## 3. 运行

### 3.1 启动

```bash
# 前台（开发）
./concurrentcache-server

# 后台
nohup ./concurrentcache-server > server.log 2>&1 &

# systemd（见 § 7）
sudo systemctl start concurrentcache
```

> 端口与配置文件都可用命令行覆盖：`--config <path>`、`--port <n>`；`--help` 列出全部选项。显式指定的 `--config` 读不到会以退出码 1 失败，而不显式指定时默认路径缺失则回落到内置默认值（从 `build/` 目录直接启动是受支持的用法）。

### 3.2 启动顺序（main.cpp 实现）

1. 提升 `RLIMIT_NOFILE` 至 `min(65535, rlim_max)`
2. 注册 `SIGINT` / `SIGTERM` 信号处理器
3. 加载 `conf/concurrentcache.conf`
4. 初始化日志
5. 启动 `SubReactorPool`（N 个 I/O 线程）
6. 启动 `MainReactor`（绑定端口）
7. **加载 RDB**（`RdbPersistence::load`）
8. 初始化 `ClusterServer`
9. 启动 `ExpirationChecker`（100ms 周期清理过期键）
10. 启动 `RdbScheduler`（按 interval/threshold 触发）
11. 启动 `ClusterServer`（如启用）
12. `MainReactor::start()`（阻塞 `epoll_wait` 循环）

### 3.3 优雅退出

`SIGINT` / `SIGTERM`：

1. `signal_handler` 设置 `g_running = false`
2. `EventLoop::quit()`（atomic store + `wakeup()`）
3. `MainReactor::start()` 返回
4. 顺序停止：`RdbScheduler` → `SubReactorPool` → `MainReactor` → `ExpirationChecker` → `ClusterServer` → `ThreadPool`
5. **强制 `RdbPersistence::save`**（保证最后写不丢）
6. 退出

## 4. 配置项

`conf/concurrentcache.conf`（仅支持 key=value 行格式，`#` 开头为注释）：

| Key | 默认 | 说明 |
|-----|------|------|
| `port` | `6379` | 客户端监听端口（程序内置默认与仓库 conf 一致） |
| `log_level` | conf 缺这个键时按 `info` 起（`config.cpp:64-65` 会注入字符串 `info`） | `0`=TRACE … `5`=FATAL，**数值越大越不详细**；conf 中的 `4` 表示只输出 ERROR/FATAL |
| `reactor_count` | CPU 核数 | SubReactor 数量 |
| `thread_pool_size` | CPU 核数 | 通用 ThreadPool 数量 |
| `rdb_path` | `./dump.rdb` | RDB 文件路径 |
| `rdb_save_interval` | `900` | 自动保存间隔（秒；配置 ≤ 0 时自动回退默认） |
| `rdb_dirty_threshold` | `1` | 达到 N 个脏键就触发保存（配置 ≤ 0 时自动回退默认） |
| `max_entries` | `2000000` | 键数量上限，触发近似 LRU 淘汰。启动时由 `main.cpp` 交给 `GlobalStorage::set_max_entries()`；缺省或 `0` 沿用内置默认，负数视为配错并回退默认。淘汰按条数判断，没有按字节的 maxmemory 口径 |
| `client_query_buffer_limit` | `16777216` | 单条连接输入缓冲高水位（字节）。未完成请求攒过这个值就断开该连接，等价于当前可用的最大 value 尺寸；`0` = 不限制，小于 1MB 抬到 1MB |
| `client_output_buffer_limit` | `67108864` | 单条连接输出缓冲高水位（字节）。对端不读时回复的积压上限，超过即断开；`0` = 不限制 |
| `cluster_enabled` | `false` | 是否启用集群模式 |
| `cluster_node_timeout` | `15000` | Gossip 节点超时（毫秒） |
| `cluster_config_file` | `nodes.conf` | 集群节点状态文件 |
| `cluster_replica_validity_factor` | `10` | 从节点失联判定倍率 |
| `cluster_require_full_coverage` | `false` | 槽位不全时是否拒绝服务 |
| `cluster_bind_addr` | `127.0.0.1` | 集群 bus 绑定地址 |
| `log_file` | `./logs/concurrentcache.log` | 日志文件路径；非空时 `main.cpp` 调 `Logger::setFile()`，日志才会落盘（为空则只出控制台） |
| `log_max_size` | `104857600`（100MB） | 单个日志文件轮转阈值，经 `Logger::setRotation()` 生效 |
| `log_max_files` | 5 | **当前未被读取**：`main.cpp` 调 `setRotation(max_bytes, 5)` 时把保留数硬编码成 5 |

**示例配置**：

```ini
port = 6379
log_level = 3
reactor_count = 16
thread_pool_size = 16
rdb_path = /var/lib/concurrentcache/dump.rdb
rdb_save_interval = 300
rdb_dirty_threshold = 1000
max_entries = 5000000
cluster_enabled = true
cluster_node_timeout = 15000
```

> 任何配置项缺失会使用默认值（`main.cpp` 中显式兜底）。

## 5. Docker 部署

### 5.1 使用镜像

本仓库的 CI 只有 `docker-build` 这一个冒烟 job（构建成功即通过），**没有推送镜像的工作流**，所以不存在可直接 `docker pull` 的预构建镜像。请先本地构建（§5.2），再运行：

```bash
docker build -t concurrentcache:latest .
docker run -d \
  --name concurrentcache \
  -p 6379:6379 \
  -v $(pwd)/data:/app/data \
  -v $(pwd)/conf/concurrentcache.conf:/app/conf/concurrentcache.conf:ro \
  --restart unless-stopped \
  concurrentcache:latest

docker exec concurrentcache redis-cli -p 6379 PING
# PONG
```

镜像基于 `debian:bookworm-slim`：

- 非 root 用户（`appuser`）
- 内置 `redis-tools`（用于健康检查）
- 镜像内 `EXPOSE 6379` + 健康检查 `redis-cli -p 6379 PING`（每 30s），与仓库默认 `conf` 的 `port = 6379` 一致。这三处（conf / `EXPOSE` / `HEALTHCHECK`）由 `scripts/ci/check_consistency.py` 强制对齐，改一处不改其余会直接让 CI 变红
- 默认 ENTRYPOINT：`/app/concurrentcache-server`，`CMD ["--config", "/app/conf/concurrentcache.conf"]` 现在真的生效（`main.cpp` 解析 `--config` / `--port` / `--help`）

### 5.2 本地构建镜像

```bash
docker build -t concurrentcache:latest .
```

构建过程（`Dockerfile`）：

1. **builder 阶段**：`gcc:12` + `cmake` + `ninja-build` + `zlib1g-dev` → cmake Release 构建
2. **runtime 阶段**：`debian:bookworm-slim` + `zlib1g` + `ca-certificates` + `redis-tools` → 复制二进制 + conf

### 5.3 Docker Compose

> 仓库根目录**没有** docker-compose.yml，以下为参考示例，需自行创建：

```yaml
# docker-compose.yml
services:
  concurrentcache:
    build: .
    image: concurrentcache:local
    ports:
      - "6379:6379"       # 客户端端口；集群总线端口 16379 需另行映射
    volumes:
      - ./data:/app/data
      - ./conf/concurrentcache.conf:/app/conf/concurrentcache.conf:ro
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "redis-cli", "-p", "6379", "PING"]
      interval: 30s
      timeout: 10s
      retries: 3
```

```bash
docker-compose up -d
```

## 6. 集群部署

### 6.1 启动多节点

每个节点使用独立配置文件，分别启动：

每个节点两个端口：客户端端口 `port`，总线端口自动等于它 + 10000。三个节点的推荐布局是客户端 6379 / 6380 / 6381（总线即 16379 / 16380 / 16381）。

```bash
# 节点 A（默认 conf，客户端端口 6379）
./concurrentcache-server --config conf/concurrentcache.conf &

# 节点 B：改 conf/node_b.conf 的 port = 6380 且 cluster_enabled = true
./concurrentcache-server --config conf/node_b.conf &

# 也可以不改配置文件，直接用命令行覆盖端口（--port 会被解析并校验 1..65535）
./concurrentcache-server --config conf/concurrentcache.conf --port 6381 &
```

> `--config` 指定的文件读不到时进程以退出码 1 失败（不静默回落）；不指定 `--config` 时默认路径缺失才回落到内置默认值。

### 6.2 加入集群

节点 A 启动后，在节点 B 上执行：

```bash
redis-cli -p 6380 CLUSTER MEET 127.0.0.1 6379
```

重复执行直到所有节点互相认识。

### 6.3 分配槽位

```bash
# 节点 A 负责 0-5460
redis-cli -p 6379 CLUSTER ADDSLOTS 0 1 2 ... 5460

# 节点 B 负责 5461-10922
redis-cli -p 6380 CLUSTER ADDSLOTS 5461 ... 10922

# 节点 C 负责 10923-16383
redis-cli -p 6381 CLUSTER ADDSLOTS 10923 ... 16383
```

### 6.4 配置主从

```bash
# 在从节点上
redis-cli -p 6382 CLUSTER REPLICATE <master-node-name>
```

详见 [集群架构 § 5 主从复制](architecture/cluster.md)。

## 7. systemd 部署

`/etc/systemd/system/concurrentcache.service`：

```ini
[Unit]
Description=ConcurrentCache Server
After=network.target

[Service]
Type=simple
User=appuser
Group=appuser
WorkingDirectory=/opt/concurrentcache
ExecStart=/opt/concurrentcache/concurrentcache-server
Restart=on-failure
RestartSec=5s
LimitNOFILE=65535

# 资源限制
MemoryMax=4G
CPUQuota=400%

# 日志
StandardOutput=journal
StandardError=journal
SyslogIdentifier=concurrentcache

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now concurrentcache
sudo systemctl status concurrentcache
```

## 8. 监控与可观测

### 8.1 INFO 命令

```bash
redis-cli -p 6379 INFO server
redis-cli -p 6379 INFO stats
redis-cli -p 6379 INFO persistence
redis-cli -p 6379 INFO keyspace
redis-cli -p 6379 INFO all
```

### 8.2 关键指标

| 指标 | 来源 | 说明 |
|------|------|------|
| `total_bgsave_calls` | `INFO stats` | BGSAVE 总调用次数 |
| `total_rdb_saved_keys` | `INFO stats` | 累计持久化的 key 数 |
| `rdb_last_bgsave_status` | `INFO persistence` | 上次 BGSAVE 状态（ok/err） |
| `rdb_last_bgsave_time_sec` | `INFO persistence` | 上次 BGSAVE 时间戳 |
| `rdb_dirty_count` | `INFO persistence` | 自上次保存起的写操作数 |
| `cluster_stats_messages_sent` / `_received` | `CLUSTER INFO` | 经过总线的消息条数（gossip 与复制数据都算）。两个数长期不对称通常意味着单向连通：我能发给某节点，但它发不到我 |
| `cluster_bus_inbound_links` / `cluster_bus_inbound_refused` | `CLUSTER INFO` | 入站链路数与因配额（512）被关掉的连接数；后者持续增长是资源耗尽的前兆 |
| `total_connections_received` / `total_commands_processed` | `INFO stats` | 进程内实时累计，重启归零；前者数成功 accept，后者数进入执行路径的命令（重定向与未知命令名不计） |
| `db0:keys=N` | `INFO keyspace` | 当前 key 总数（含已过期未删除的条目） |
| `used_memory_rss` | `INFO memory` | 进程常驻内存（内核视角，来自 `/proc/self/status` 的 VmRSS）。**没有** `used_memory`：内存池未接入分配路径，没有可信的分配器字节数可报 |
| `maxmemory_policy` | `INFO memory` | 淘汰口径：按条数做随机分片采样近似 LRU，不是按字节 |

### 8.3 调试命令

```bash
redis-cli -p 6379 DEBUG OBJECT key   # 查看对象类型
# 注意：DEBUG SLEEP 已被禁用（会阻塞事件循环），返回 -ERR DEBUG SLEEP is not supported
```

## 9. 故障排查（Runbook）

### 9.1 启动失败

| 症状 | 排查 |
|------|------|
| 端口占用 | `lsof -i :6379` / `ss -tlnp \| grep 6379`（别忘了总线端口 16379 = 客户端端口 + 10000，它也要放行） |
| 配置文件语法错 | 检查 `conf/concurrentcache.conf` 每行 `key = value` 格式 |
| ZLIB 未找到 | `apt install zlib1g-dev`（构建时）/ `zlib1g`（运行时） |
| C++20 报错 | `g++ --version`（需 ≥ 12） |

### 9.2 运行期

| 症状 | 排查 |
|------|------|
| 连接被拒 | 检查 `port` / 防火墙 / 是否启动 |
| 大量 timeout | 检查 `reactor_count` 是否小于 CPU 核数；查 `dirty_count` 是否持续高位 |
| 内存持续上涨 | 检查 `max_entries` 配置；触发 `BGSAVE` |
| RDB 加载失败 | 删除 `dump.rdb` 重启（数据无法恢复时） |
| 集群节点失联 | `CLUSTER NODES` 看 FAIL/PFAIL 标志；检查 `cluster_node_timeout` |
| 故障转移卡住 | 查 `cluster_bus_` 端口（`port + 10000`）是否可达 |

### 9.3 调试死锁 / 挂起

```bash
# 1. 找到进程
ps -ef | grep concurrentcache

# 2. gdb attach
gdb -p <pid>
(gdb) thread apply all bt
(gdb) info threads
(gdb) detach
```

各线程预期状态：

| 线程 | 预期阻塞点 |
|------|----------|
| MainReactor | `epoll_wait`（`EventLoop::loop()`） |
| SubReactor ×N | `epoll_wait` |
| ThreadPool ×N | `condition_variable.wait` |
| ExpirationChecker | `std::this_thread::sleep_for(100ms)` |
| RdbScheduler | `std::this_thread::sleep_for(1s)` |
| ClusterConnection 心跳 | `sleep_for(1s)`（该线程驱动 `on_timer()` 与 `executeFailover()`，不是复用某个 EventLoop） |
| ClusterBus 链路 | 由心跳线程与各 link 的 fd 直接收发，不额外占 reactor 线程 |

### 9.4 数据恢复

```bash
# 手动从 RDB 恢复——启动时自动加载
./concurrentcache-server

# 强制保存
redis-cli -p 6379 SAVE    # 同步保存
redis-cli -p 6379 BGSAVE  # 异步保存
```

## 10. 升级与回滚

### 10.1 升级步骤

```bash
# 1. 停止服务
sudo systemctl stop concurrentcache

# 2. 备份 RDB（关键）
cp /var/lib/concurrentcache/dump.rdb /backup/dump.$(date +%Y%m%d).rdb

# 3. 替换二进制
cp /tmp/concurrentcache-server /opt/concurrentcache/

# 4. 启动
sudo systemctl start concurrentcache

# 5. 验证
redis-cli -p 6379 PING
redis-cli -p 6379 DBSIZE
```

### 10.2 回滚

```bash
sudo systemctl stop concurrentcache
cp /backup/dump.20260601.rdb /var/lib/concurrentcache/dump.rdb
# 恢复旧版二进制
sudo systemctl start concurrentcache
```

## 11. 性能基准

对照由 `daily.yml` 的 `redis-compat` job 每晚执行：同一台 GitHub Actions `ubuntu-24.04` runner 上
拉起本项目的 Release 二进制与 `redis-server 7.0.15`（`5:7.0.15-1ubuntu0.24.04.5`，Ubuntu 24.04
仓库版本），逐场景测 QPS 与 p99。并发阶梯的命令配比是 70% 读 / 10% 写 / 20% 删除，单连接三行的
读占比分别是 95% / 0% / 70%（脚本里的「纯 GET」实际是 95% 读）；1KB–100KB 那六行统一在并发=100
下测，其中 SET 行是纯写、GET 行是 95% 读。
下表是 **2026-10-09 nightly（被测 `5bdcc82`、Release 构建、64 分片）** 的那一轮：

| 场景 | ConcurrentCache | Redis | CC/Redis |
|------|----------------|-------|----------|
| 纯 GET（单连接） | 21,093 QPS | 29,785 QPS | 71% |
| 纯 SET（单连接） | 28,882 QPS | 29,262 QPS | 99% |
| 混合负载（单连接） | 27,798 QPS | 29,096 QPS | 96% |
| 并发=10 混合 | 59,366 QPS | 72,795 QPS | 82% |
| 并发=50 混合 | 62,201 QPS | 72,281 QPS | 86% |
| 并发=100 混合 | 60,184 QPS | 71,911 QPS | 84% |
| 并发=200 混合 | 63,734 QPS | 72,942 QPS | 87% |
| 并发=500 混合 | 58,235 QPS | 72,053 QPS | 81% |
| 并发=1000 混合 | 53,897 QPS | 68,587 QPS | 79% |
| 并发=2000 混合 | 38,594 QPS | 52,404 QPS | 74% |
| 并发=3000 混合 | 36,619 QPS | 41,237 QPS | 89% |
| 并发=5000 混合 | 32,601 QPS | 37,005 QPS | 88% |
| SET 1KB value | 67,274 QPS | 77,362 QPS | 87% |
| GET 1KB value | 64,311 QPS | 75,876 QPS | 85% |
| SET 10KB value | 62,229 QPS | 75,491 QPS | 82% |
| GET 10KB value | 59,052 QPS | 70,188 QPS | 84% |
| SET 100KB value | 52,669 QPS | 63,447 QPS | 83% |
| GET 100KB value | 44,036 QPS | 64,375 QPS | 68% |

> 平均 QPS 比率：**83.5%**（最低 68.4%，最高 98.7%；峰值 QPS CC=67,274 / Redis=77,362）。
> 单连接场景贴近 Redis（71%–99%），高并发混合负载稳定在 79%–89%；大 value 的读（GET 100KB，68%）
> 是目前最差的一栏。
>
> 内存那两行（空载 / 满载 RSS）**两边都采到收敛为止**（各最多 20 轮 × 0.5s，容差 2%，
> 基准取窗口起点，所以持续下降不会被当成稳定）；任何一边没收敛，`每 key 开销` 就明写
> "不可用"而不是给个数。之所以要这么麻烦：`FLUSHDB` 只删逻辑条目，glibc 把堆还给内核是
> 延迟且不完全的，单次采样会拿到一个还在下降的值，于是出现过"满载比空载低几百 MB"。
> 这栏数字同时也是本服务器**不主动 `malloc_trim`、内存池未接线**的直接后果 ——
> 属于部署时要考虑的事，不是测量噪声。
>
> **绝对值只在同一次测量内部可比。** runner 是共享虚拟化环境，QPS 与 p99 会随邻居负载浮动，
> 并发=2000 以上的行尤其不稳。要引用绝对数字请连同被测 sha 一起记下来；只有 CC/Redis 比值和
> 「两边同一套用例的通过率」适合作为跨时间的对照。
>
> 压力测试段的 `err=` 列（并发场景 69%–100%）以及「大量 key 写入 50000」两栏同时判失败，来自
> 对比脚本自己的回复配对错位，不是服务端错误率 —— 同一份报告里真 Redis 也吃同样的错。#99 修掉了
> 第一层（每条命令单独 read 并丢弃剩余字节）；2026-10-10 的 nightly 上「大量 key 写入」两栏已经
> 转绿，但暴露出第二层：RESP 的 nil（`$-1`）在客户端里也映射成 Python `None`，而读循环只在
> 结果非 None 时才推进缓冲区，于是**任何一条 nil 之后整条连接永久读不到回复**。表里那行
> 「功能正确性」当时是 39/39，等 nil 分支被踩到（`HGET` 一个不存在的字段）就变成两边一起红的
> 38/39。第二层已修，并由 `test_resp_client.py` 的 `nil-then-more-replies` 断言钉住
> （它在旧代码上会失败，症状与 nightly 完全一致）。
>
> 本节之前记的是「8 核、平均 90.4%，并发=10 时 108%、SET 1KB 时 104%」。那组数字没有留下原始
> 报告与被测 sha，也无法在当前 CI 上复现；换成 nightly 实测后比值整体下移了约 7 个百分点，
> 这是**测量口径变化 + 原本没有复现路径**的结果，不代表这段时间的性能回退。

复现：`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target concurrentcache-server`
后执行 `python3 test/e2e_test/comparison_test.py`（需本机有 `redis-server`），报告落在
`test/e2e_test/comparison_report.json`。

## 12. 另见

- [架构总览](./architecture/overview.md) — 系统组成
- [持久化架构](./architecture/persistence.md) — RDB 机制
- [集群架构](./architecture/cluster.md) — 集群协议
- [测试文档 § CI 集成](./testing.md)
- [测试文档 § Sanitizer](./testing.md)