# 部署与运维

> **目标平台**：Linux x86_64（推荐 Ubuntu 24.04 / Debian 12）
> **部署方式**：源码编译 / Docker / Docker Compose
> **默认端口**：`16379`（客户端）+ `26379`（集群总线，`port + 10000`）
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
git clone https://github.com/dingziming/ConcurrentCache.git
cd ConcurrentCache
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

**产物**：`build/concurrentcache-server`（约 4MB Release 二进制）

### 2.4 启用 Sanitizer

```bash
# ASan（推荐先跑一遍）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build

# TSan（重点查数据竞争）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build

# UBSan
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON
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

> 当前启动方式固定读取 `conf/concurrentcache.conf`，不支持 `--port` 或 `--config` 命令行参数。如需修改端口，编辑配置文件后重启。

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
| `port` | `16379` | 客户端监听端口 |
| `log_level` | `4` | 日志级别（数值越大越详细） |
| `reactor_count` | CPU 核数 | SubReactor 数量 |
| `thread_pool_size` | CPU 核数 | 通用 ThreadPool 数量 |
| `rdb_path` | `./dump.rdb` | RDB 文件路径 |
| `rdb_save_interval` | `900` | 自动保存间隔（秒） |
| `rdb_dirty_threshold` | `1` | 达到 N 个脏键就触发保存 |
| `max_entries` | `2000000` | 存储上限（触发 ARU 淘汰） |
| `cluster_enabled` | `false` | 是否启用集群模式 |
| `cluster_node_timeout` | `5000` | Gossip 节点超时（毫秒） |

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
cluster_node_timeout = 5000
```

> 任何配置项缺失会使用默认值（`main.cpp` 中显式兜底）。

## 5. Docker 部署

### 5.1 使用预构建镜像

```bash
docker pull ghcr.io/dingziming/concurrentcache:latest
docker run -d \
  --name concurrentcache \
  -p 16379:16379 \
  -v $(pwd)/data:/app/data \
  -v $(pwd)/conf/concurrentcache.conf:/app/conf/concurrentcache.conf:ro \
  --restart unless-stopped \
  ghcr.io/dingziming/concurrentcache:latest

docker exec concurrentcache redis-cli -p 16379 PING
# PONG
```

镜像基于 `debian:bookworm-slim`：

- 非 root 用户（`appuser`）
- 内置 `redis-tools`（用于健康检查）
- 健康检查：`redis-cli -p 16379 PING` 每 30s
- 默认 ENTRYPOINT：`/app/concurrentcache-server`

### 5.2 本地构建镜像

```bash
docker build -t concurrentcache:latest .
```

构建过程（`Dockerfile`）：

1. **builder 阶段**：`gcc:14` + `cmake` + `ninja-build` + `zlib1g-dev` → cmake Release 构建
2. **runtime 阶段**：`debian:bookworm-slim` + `zlib1g` + `ca-certificates` + `redis-tools` → 复制二进制 + conf

### 5.3 Docker Compose

```yaml
# docker-compose.yml
services:
  concurrentcache:
    image: ghcr.io/dingziming/concurrentcache:latest
    ports:
      - "16379:16379"
    volumes:
      - ./data:/app/data
      - ./conf/concurrentcache.conf:/app/conf/concurrentcache.conf:ro
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "redis-cli", "-p", "16379", "PING"]
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

```bash
# 节点 A（默认配置，端口 16379）
./concurrentcache-server &

# 节点 B（准备 conf/node_b.conf，port=16380）
cp conf/concurrentcache.conf conf/node_b.conf
# 修改 conf/node_b.conf 中 port = 16380
# 修改 conf/node_b.conf 中 cluster_enabled = true
./concurrentcache-server &
```

> **注意**：当前不支持 `--port` 命令行参数，多节点部署需准备独立配置文件。

### 6.2 加入集群

节点 A 启动后，在节点 B 上执行：

```bash
redis-cli -p 16380 CLUSTER MEET 127.0.0.1 16379
```

重复执行直到所有节点互相认识。

### 6.3 分配槽位

```bash
# 节点 A 负责 0-5460
redis-cli -p 16379 CLUSTER ADDSLOTS 0 1 2 ... 5460

# 节点 B 负责 5461-10922
redis-cli -p 16380 CLUSTER ADDSLOTS 5461 ... 10922

# 节点 C 负责 10923-16383
redis-cli -p 16381 CLUSTER ADDSLOTS 10923 ... 16383
```

### 6.4 配置主从

```bash
# 在从节点上
redis-cli -p 16384 CLUSTER REPLICATE <master-node-name>
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
redis-cli -p 16379 INFO server
redis-cli -p 16379 INFO stats
redis-cli -p 16379 INFO persistence
redis-cli -p 16379 INFO keyspace
redis-cli -p 16379 INFO all
```

### 8.2 关键指标

| 指标 | 来源 | 说明 |
|------|------|------|
| `total_bgsave_calls` | `INFO stats` | BGSAVE 总调用次数 |
| `total_rdb_saved_keys` | `INFO stats` | 累计持久化的 key 数 |
| `rdb_last_bgsave_status` | `INFO persistence` | 上次 BGSAVE 状态（ok/err） |
| `rdb_last_bgsave_time_sec` | `INFO persistence` | 上次 BGSAVE 时间戳 |
| `rdb_dirty_count` | `INFO persistence` | 自上次保存起的写操作数 |
| `db0:keys=N` | `INFO keyspace` | 当前 key 总数 |

### 8.3 调试命令

```bash
redis-cli -p 16379 DEBUG SLEEP 5     # 模拟慢请求
redis-cli -p 16379 DEBUG OBJECT key   # 查看对象类型
```

## 9. 故障排查（Runbook）

### 9.1 启动失败

| 症状 | 排查 |
|------|------|
| 端口占用 | `lsof -i :16379` / `ss -tlnp \| grep 16379` |
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
| ClusterServer Timer | `epoll_wait`（复用 MainReactor loop） |

### 9.4 数据恢复

```bash
# 手动从 RDB 恢复——启动时自动加载
./concurrentcache-server

# 强制保存
redis-cli -p 16379 SAVE    # 同步保存
redis-cli -p 16379 BGSAVE  # 异步保存
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
redis-cli -p 16379 PING
redis-cli -p 16379 DBSIZE
```

### 10.2 回滚

```bash
sudo systemctl stop concurrentcache
cp /backup/dump.20260601.rdb /var/lib/concurrentcache/dump.rdb
# 恢复旧版二进制
sudo systemctl start concurrentcache
```

## 11. 性能基准

以下数据基于 **8 核 Linux x86_64、Release 构建、64 分片、对比 Redis 7.0.15** 实测：

| 场景 | ConcurrentCache | Redis | CC/Redis |
|------|----------------|-------|----------|
| 纯 GET（单连接） | 32,863 QPS | 55,224 QPS | 60% |
| 纯 SET（单连接） | 42,318 QPS | 55,099 QPS | 77% |
| 混合负载（单连接） | 42,640 QPS | 58,728 QPS | 73% |
| 并发=10 混合 | 89,077 QPS | 82,397 QPS | **108%** |
| 并发=100 混合 | 85,430 QPS | 87,734 QPS | 97% |
| 并发=1000 混合 | 75,425 QPS | 75,438 QPS | 100% |
| 并发=5000 混合 | 57,073 QPS | 62,301 QPS | 92% |
| SET 1KB（并发=100） | 91,412 QPS | 88,058 QPS | **104%** |

> 平均 QPS 比率：**90.4%**。单连接场景偏弱（60-77%），高并发场景与 Redis 持平（92-108%）。

## 12. 另见

- [架构总览](./architecture/overview.md) — 系统组成
- [持久化架构](./architecture/persistence.md) — RDB 机制
- [集群架构](./architecture/cluster.md) — 集群协议
- [测试文档 § CI 集成](./testing.md)
- [测试文档 § Sanitizer](./testing.md)