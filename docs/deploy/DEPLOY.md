# ConcurrentCache 部署手册

> **文档版本**：v3.2 · 适用服务端版本 V3.0+
> **目标平台**：Linux x86_64（推荐 Ubuntu 22.04+ / Debian 12+）
> **部署方式**：源码编译 / Docker / Docker Compose

---

## 📑 目录

- [1. 概览](#1-概览)
- [2. 系统要求](#2-系统要求)
- [3. 部署方式选择](#3-部署方式选择)
- [4. 源码编译部署](#4-源码编译部署)
- [5. Docker 部署](#5-docker-部署)
- [6. Docker Compose 部署](#6-docker-compose-部署)
- [7. 配置文件详解](#7-配置文件详解)
- [8. 启动与验证](#8-启动与验证)
- [9. 集群部署](#9-集群部署)
- [10. 监控与日志](#10-监控与日志)
- [11. 备份与恢复](#11-备份与恢复)
- [12. 性能调优](#12-性能调优)
- [13. 升级与回滚](#13-升级与回滚)
- [14. 故障 Runbook](#14-故障-runbook)
- [15. 安全加固](#15-安全加固)
- [16. 另见](#16-另见)

---

## 1. 概览

ConcurrentCache 提供三种部署形态：

| 形态 | 适用场景 | 启动时间 | 隔离性 |
|------|---------|---------|--------|
| **源码编译** | 开发调试、定制编译选项 | 数分钟 | 低 |
| **Docker** | 生产环境、CI/CD | 秒级 | 高 |
| **Docker Compose** | 单机多实例、集群测试 | 秒级 | 高 |

---

## 2. 系统要求

### 2.1 硬件最低配置

| 资源 | 最低 | 推荐 |
|------|------|------|
| CPU | 2 核 | 8 核+ |
| 内存 | 1 GB | 8 GB+（按数据量估算） |
| 磁盘 | 1 GB | 50 GB+（持久化） |
| 网络 | 100 Mbps | 1 Gbps+ |

### 2.2 软件依赖

**源码编译依赖**：

| 工具 | 最低版本 | 验证命令 |
|------|---------|---------|
| GCC | 12 | `gcc --version` |
| Clang | 16（可选） | `clang --version` |
| CMake | 3.20 | `cmake --version` |
| ZLIB | 任意 | `ldconfig -p \| grep libz` |
| pthread | 系统自带 | — |

**Docker 依赖**：

| 工具 | 版本 | 说明 |
|------|------|------|
| Docker Engine | 20.10+ | 容器运行时 |
| Docker Compose | v2.0+ | 多容器编排（可选） |

### 2.3 操作系统

| 系统 | 支持 | 测试版本 |
|------|------|---------|
| Ubuntu | ✅ | 22.04 / 24.04 |
| Debian | ✅ | 12 (bookworm) |
| CentOS / RHEL | ⚠️ 自行验证 | 8+ |
| macOS | ❌ 不支持（依赖 epoll） | — |
| Windows | ❌ 不支持 | — |

---

## 3. 部署方式选择

```
你打算做什么？
│
├─ 本地开发调试
│   └─ 选：源码编译（GCC 14 + ninja + ASan）
│
├─ 生产环境单机
│   └─ 选：Docker（多阶段镜像，非 root 运行）
│
├─ 单机多端口实例
│   └─ 选：Docker Compose
│
└─ 多机集群
    └─ 选：源码编译 / Docker × 多节点
```

---

## 4. 源码编译部署

### 4.1 安装编译依赖

**Ubuntu / Debian**：

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    build-essential \
    zlib1g-dev \
    git
```

### 4.2 拉取代码

```bash
git clone https://github.com/dingziming/ConcurrentCache.git
cd ConcurrentCache
```

### 4.3 编译 Release 版本

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

### 4.4 编译 Debug + Sanitizer 版本（开发用）

```bash
mkdir -p build-debug && cd build-debug
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build . --parallel $(nproc)
```

可选 sanitizer：

- `-DENABLE_ASAN=ON` — AddressSanitizer（内存越界/UAF 检测）
- `-DENABLE_TSAN=ON` — ThreadSanitizer（数据竞争检测）
- `-DENABLE_UBSAN=ON` — UndefinedBehaviorSanitizer

> 三种 sanitizer **不可同时启用**。

### 4.5 产物位置

```
build/
├── concurrentcache-server     # 主服务可执行文件
├── test/                      # 测试可执行文件
│   ├── concurrentcache-v3-tests
│   ├── atomic-tests
│   ├── lock-*-tests
│   └── ...
└── CMakeCache.txt
```

### 4.6 创建系统用户（生产推荐）

```bash
sudo useradd -m -s /bin/bash ccuser
sudo mkdir -p /opt/concurrentcache/{bin,conf,data,logs}
sudo cp build/concurrentcache-server /opt/concurrentcache/bin/
sudo cp conf/concurrentcache.conf /opt/concurrentcache/conf/
sudo chown -R ccuser:ccuser /opt/concurrentcache
```

### 4.7 配置 systemd 服务

创建 `/etc/systemd/system/concurrentcache.service`：

```ini
[Unit]
Description=ConcurrentCache - High-performance in-memory cache
After=network.target

[Service]
Type=simple
User=ccuser
Group=ccuser
WorkingDirectory=/opt/concurrentcache
ExecStart=/opt/concurrentcache/bin/concurrentcache-server --config /opt/concurrentcache/conf/concurrentcache.conf
Restart=on-failure
RestartSec=5s

# 资源限制
LimitNOFILE=65535
LimitNPROC=65535

# 日志
StandardOutput=append:/opt/concurrentcache/logs/stdout.log
StandardError=append:/opt/concurrentcache/logs/stderr.log

[Install]
WantedBy=multi-user.target
```

**启用与启动**：

```bash
sudo systemctl daemon-reload
sudo systemctl enable concurrentcache
sudo systemctl start concurrentcache
sudo systemctl status concurrentcache
```

**查看日志**：

```bash
sudo journalctl -u concurrentcache -f
# 或
tail -f /opt/concurrentcache/logs/stdout.log
```

---

## 5. Docker 部署

### 5.1 拉取预构建镜像

```bash
docker pull ghcr.io/dingziming/concurrentcache:latest
```

### 5.2 启动容器

```bash
docker run -d \
  --name concurrentcache \
  -p 6379:6379 \
  -v /path/on/host/data:/app/data \
  -v /path/on/host/conf:/app/conf:ro \
  --restart unless-stopped \
  ghcr.io/dingziming/concurrentcache:latest
```

**参数说明**：

| 参数 | 含义 |
|------|------|
| `-d` | 后台运行 |
| `-p 6379:6379` | 映射 6379 端口 |
| `-v .../data:/app/data` | 持久化目录挂载 |
| `-v .../conf:/app/conf:ro` | 配置文件只读挂载 |
| `--restart unless-stopped` | 自动重启 |

### 5.3 本地构建镜像

```bash
# 在项目根目录
docker build -t concurrentcache:custom .
```

Dockerfile 采用**多阶段构建**：

- Stage 1（builder）：`gcc:14` 镜像，编译出二进制
- Stage 2（runtime）：`debian:bookworm-slim`，仅包含运行时依赖 + 二进制

### 5.4 验证容器状态

```bash
# 查看运行状态
docker ps | grep concurrentcache

# 健康检查（镜像内置）
docker inspect --format='{{json .State.Health}}' concurrentcache

# 查看日志
docker logs -f concurrentcache
```

### 5.5 容器内置健康检查

镜像 Dockerfile 中已配置：

```dockerfile
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD redis-cli -p 6379 PING > /dev/null 2>&1 || exit 1
```

每 30 秒自动 PING 一次，失败 3 次标记为 unhealthy。

### 5.6 容器内操作

```bash
# 进入容器调试
docker exec -it concurrentcache /bin/bash

# 容器内自带 redis-cli
docker exec -it concurrentcache redis-cli -p 6379 PING
```

---

## 6. Docker Compose 部署

### 6.1 单实例版（最简）

`docker-compose.yml`：

```yaml
version: '3.8'
services:
  concurrentcache:
    image: ghcr.io/dingziming/concurrentcache:latest
    container_name: concurrentcache
    ports:
      - "6379:6379"
    volumes:
      - ./data:/app/data
      - ./conf:/app/conf:ro
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "redis-cli", "-p", "6379", "PING"]
      interval: 30s
      timeout: 10s
      retries: 3
      start_period: 5s
```

**启动**：

```bash
docker-compose up -d
docker-compose ps
docker-compose logs -f
```

**停止**：

```bash
docker-compose down           # 停止并删除容器
docker-compose down -v        # 同时删除挂载卷
```

### 6.2 集群版（3 主 3 从）

```yaml
version: '3.8'
services:
  cc-node-1:
    image: ghcr.io/dingziming/concurrentcache:latest
    container_name: cc-node-1
    ports:
      - "6379:6379"
      - "16379:16379"   # 集群总线端口
    volumes:
      - ./data/node-1:/app/data
    command: ["--config", "/app/conf/concurrentcache.conf"]
    networks:
      - cc-net

  cc-node-2:
    image: ghcr.io/dingziming/concurrentcache:latest
    container_name: cc-node-2
    ports:
      - "6380:6379"
      - "16380:16379"
    volumes:
      - ./data/node-2:/app/data
    networks:
      - cc-net

  cc-node-3:
    image: ghcr.io/dingziming/concurrentcache:latest
    container_name: cc-node-3
    ports:
      - "6381:6379"
      - "16381:16379"
    volumes:
      - ./data/node-3:/app/data
    networks:
      - cc-net

networks:
  cc-net:
    driver: bridge
```

> 集群节点握手配置见 [9. 集群部署](#9-集群部署)。

---

## 7. 配置文件详解

文件路径：

- 源码部署：`/opt/concurrentcache/conf/concurrentcache.conf`
- Docker 部署：容器内 `/app/conf/concurrentcache.conf`

### 7.1 完整配置示例

```ini
# ============ 网络 ============
port = 6379

# ============ 线程模型 ============
# 不设置则自动获取 CPU 核心数
# reactor_count = 32
# thread_pool_size = 32

# ============ 日志 ============
# 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR
log_level = 3
config_check_interval = 10

# ============ RDB 持久化 ============
rdb_path = ./dump.rdb
# 0 = 禁用自动保存；> 0 = 每隔 N 秒自动保存
rdb_save_interval = 0
# 脏键数达到此值时强制保存
rdb_dirty_threshold = 10000

# ============ 容量 ============
max_entries = 2000000

# ============ 集群（V4.0）============
cluster_enabled = false
cluster_config_file = nodes.conf
cluster_node_timeout = 15000
cluster_replica_validity_factor = 10
cluster_require_full_coverage = false
```

### 7.2 配置项逐项说明

| 配置项 | 默认值 | 取值范围 | 含义 | 调优建议 |
|--------|-------|---------|------|---------|
| `port` | 6379 | 1-65535 | 服务监听端口 | 默认即可 |
| `reactor_count` | CPU 核数 | 1-128 | SubReactor 线程数 | 一般等于 CPU 核数；超线程可设为 2 倍核数 |
| `thread_pool_size` | CPU 核数 | 1-256 | 后台任务线程数 | 同上 |
| `log_level` | 3 | 0-4 | 日志级别 | 生产 3-4；调试 1-2 |
| `config_check_interval` | 10 | 1-3600 | 配置热更新检测（秒） | 默认即可 |
| `rdb_path` | `./dump.rdb` | 任意可写路径 | RDB 快照路径 | 建议放在专用数据盘 |
| `rdb_save_interval` | 0 | 0 或 ≥60 | 自动保存间隔（秒） | 0=禁用；推荐 900-3600 |
| `rdb_dirty_threshold` | 10000 | 1-10亿 | 强制保存的脏键阈值 | 数据量大可调高 |
| `max_entries` | 2000000 | 1-10亿 | 内存中最大 key 数 | 按可用内存估算 |
| `cluster_enabled` | false | bool | 是否开启集群模式 | 多机部署开 |
| `cluster_node_timeout` | 15000 | 1000-60000 | 节点超时（毫秒） | 默认即可 |
| `cluster_require_full_coverage` | false | bool | 是否要求全部槽覆盖 | 生产建议 true |

### 7.3 热更新

服务会每 `config_check_interval` 秒重读配置文件并应用变更。**不支持热改的项**：端口、集群开关（需重启）。

---

## 8. 启动与验证

### 8.1 启动

**源码部署**：

```bash
./concurrentcache-server --config /opt/concurrentcache/conf/concurrentcache.conf
```

**Docker**：

```bash
docker start concurrentcache
```

**systemd**：

```bash
sudo systemctl start concurrentcache
```

### 8.2 验证服务存活

```bash
# PING 测试
redis-cli -h 127.0.0.1 -p 6379 PING
# 预期输出：PONG

# 写读验证
redis-cli -p 6379 SET hello world
redis-cli -p 6379 GET hello
# 预期输出："world"

# 服务信息
redis-cli -p 6379 INFO server
```

### 8.3 查看日志

**源码部署**：

- stdout / stderr 直接到终端
- systemd 模式：`journalctl -u concurrentcache -f`

**Docker**：

```bash
docker logs -f concurrentcache
```

### 8.4 查看端口

```bash
sudo ss -tlnp | grep 6379
# 预期：LISTEN 0  128  0.0.0.0:6379  ... users:(("concurrentcache-server",pid=...))
```

---

## 9. 集群部署

> 集群功能由 V4.0 启用，需要 `cluster_enabled = true`。

### 9.1 节点规划

**最小集群（3 主 0 从，仅测试）**：

| 节点 | IP | 端口 | 集群总线 |
|------|-----|------|---------|
| node-1 | 10.0.0.1 | 6379 | 16379 |
| node-2 | 10.0.0.2 | 6379 | 16379 |
| node-3 | 10.0.0.3 | 6379 | 16379 |

**生产推荐（3 主 3 从）**：每个主节点配 1 个从节点，从节点 IP 不同。

### 9.2 端口要求

| 用途 | 端口 |
|------|------|
| 客户端连接 | 6379（可改） |
| 集群总线（Gossip/复制） | 16379（= 服务端口 + 10000） |

防火墙需放行 **两个端口**。

### 9.3 启动节点

每台机器：

```bash
./concurrentcache-server --config conf/cluster-node.conf
```

`cluster-node.conf`（节点 1 示例）：

```ini
port = 6379
cluster_enabled = true
cluster_config_file = nodes-1.conf
cluster_node_timeout = 15000
```

### 9.4 握手组成集群

在 **node-1** 上执行：

```bash
# 加入 node-2
redis-cli -p 6379 CLUSTER MEET 10.0.0.2 6379

# 加入 node-3
redis-cli -p 6379 CLUSTER MEET 10.0.0.3 6379
```

### 9.5 分配哈希槽

16384 个槽分配给 3 个主节点：

```bash
# node-1 分 0-5460
redis-cli -p 6379 CLUSTER ADDSLOTS 0 1 2 ... 5460

# node-2 分 5461-10922
redis-cli -p 6379 CLUSTER ADDSLOTS 5461 ... 10922

# node-3 分 10923-16383
redis-cli -p 6379 CLUSTER ADDSLOTS 10923 ... 16383
```

> 也可使用自动化脚本 `redis-cli --cluster create` 辅助分配（仅当服务端兼容时）。

### 9.6 配置主从

```bash
# 在从节点执行：复制 node-1
redis-cli -h 10.0.0.4 -p 6379 CLUSTER REPLICATE <node-1-id>
```

`<node-1-id>` 通过 `CLUSTER NODES` 命令获取。

### 9.7 验证集群状态

```bash
redis-cli -p 6379 CLUSTER INFO
# 预期：cluster_state:ok

redis-cli -p 6379 CLUSTER NODES
# 预期：列出全部节点，含主从关系
```

### 9.8 客户端连接

使用支持集群的客户端（如 `redis-py` Cluster、`jedis` Cluster）：

```python
from redis.cluster import RedisCluster
rc = RedisCluster(host='10.0.0.1', port=6379)
rc.set('key', 'value')   # 自动路由到对应槽
```

---

## 10. 监控与日志

### 10.1 关键指标（通过 `INFO` 命令获取）

| 指标 | 命令 | 含义 |
|------|------|------|
| `total_connections_received` | `INFO stats` | 累计接收连接数 |
| `total_commands_processed` | `INFO stats` | 累计处理命令数 |
| `rdb_last_bgsave_status` | `INFO persistence` | 上次 BGSAVE 状态（ok/err） |
| `rdb_dirty_count` | `INFO persistence` | 当前脏键数 |
| `db0:keys` | `INFO keyspace` | 当前 key 数量 |

**示例**：

```bash
redis-cli -p 6379 INFO stats
redis-cli -p 6379 INFO persistence
redis-cli -p 6379 INFO keyspace
```

### 10.2 接入 Prometheus（推荐）

> 当前版本未内置 `/metrics` 端点。可通过以下方式接入：

1. 外部 exporter 周期调用 `INFO` 并转换为 Prometheus 格式
2. 自建 sidecar 解析日志

### 10.3 告警建议

| 触发条件 | 级别 | 含义 |
|---------|------|------|
| `PING` 失败 | P0 | 服务不可用 |
| `rdb_last_bgsave_status = err` | P1 | 持久化失败 |
| `rdb_dirty_count` 持续 > `rdb_dirty_threshold × 5` | P1 | 写多导致 BGSAVE 跟不上 |
| 客户端连接数近 `ulimit` | P2 | 接近资源上限 |
| `DBSIZE` 接近 `max_entries` | P2 | 容量将满 |

### 10.4 日志轮转

**systemd 模式**：使用 `journald`，无需手动轮转。

**Docker 模式**：使用 Docker logging driver：

```bash
docker run -d \
  --log-driver json-file \
  --log-opt max-size=10m \
  --log-opt max-file=3 \
  ...
```

**源码部署**：

```bash
# 用 logrotate
cat > /etc/logrotate.d/concurrentcache <<EOF
/opt/concurrentcache/logs/*.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    postrotate
        systemctl reload concurrentcache
    endscript
}
EOF
```

---

## 11. 备份与恢复

### 11.1 备份策略

**方式 1：直接复制 RDB 文件**（推荐）

```bash
# 1. 触发一次 SAVE（确保数据落盘）
redis-cli -p 6379 SAVE

# 2. 复制 RDB 文件到备份目录
cp /opt/concurrentcache/data/dump.rdb /backup/cc-$(date +%Y%m%d).rdb
```

**方式 2：滚动备份（保留最近 7 天）**：

```bash
cat > /opt/concurrentcache/backup.sh <<'EOF'
#!/bin/bash
DATA_DIR=/opt/concurrentcache/data
BACKUP_DIR=/backup/concurrentcache
DATE=$(date +%Y%m%d-%H%M%S)

mkdir -p $BACKUP_DIR
redis-cli -p 6379 SAVE > /dev/null
cp $DATA_DIR/dump.rdb $BACKUP_DIR/dump-$DATE.rdb

# 清理 7 天前的备份
find $BACKUP_DIR -name "dump-*.rdb" -mtime +7 -delete
EOF
chmod +x /opt/concurrentcache/backup.sh
```

**cron 定时**（每天凌晨 3 点）：

```bash
0 3 * * * /opt/concurrentcache/backup.sh
```

### 11.2 恢复

**从 RDB 备份恢复**：

```bash
# 1. 停止服务
sudo systemctl stop concurrentcache

# 2. 替换 RDB 文件
cp /backup/cc-20260601.rdb /opt/concurrentcache/data/dump.rdb
sudo chown ccuser:ccuser /opt/concurrentcache/data/dump.rdb

# 3. 启动服务（自动加载）
sudo systemctl start concurrentcache

# 4. 验证
redis-cli -p 6379 DBSIZE
```

**Docker 恢复**：

```bash
docker stop concurrentcache
docker cp /backup/cc-20260601.rdb concurrentcache:/app/data/dump.rdb
docker start concurrentcache
```

### 11.3 灾难恢复清单

| 项 | 清单 |
|---|------|
| **RDB 备份保留期** | 至少 7 天，建议 30 天 |
| **异地备份** | 同步到 OSS / S3 / 异地机房 |
| **恢复演练** | 季度演练一次，验证备份可用 |
| **备份加密** | 敏感数据场景使用 GPG 加密后存储 |

---

## 12. 性能调优

### 12.1 系统层

| 项 | 调优 |
|---|------|
| **文件描述符** | `ulimit -n 65535` 或 `/etc/security/limits.conf` |
| **TCP 缓冲区** | `sysctl -w net.core.rmem_max=16777216` `wmem_max=16777216` |
| **somaxconn** | `sysctl -w net.core.somaxconn=4096` |
| **TIME_WAIT 复用** | `sysctl -w net.ipv4.tcp_tw_reuse=1` |
| **CPU 亲和性** | taskset 绑定 SubReactor 到独立核心 |

### 12.2 应用层

| 项 | 调优 |
|---|------|
| `reactor_count` | 一般等于物理核数；超线程可设为 2 倍 |
| `thread_pool_size` | 同上 |
| `log_level` | 生产建议 3（WARN）或 4（ERROR） |
| `rdb_save_interval` | 高写场景调大或设为 0（避免 BGSAVE 抢占 IO） |
| `max_entries` | 按 `(可用内存 × 0.7) / 单 key 平均占用` 估算 |

### 12.3 客户端层

- **连接池**：使用连接池复用 TCP 连接，避免频繁握手
- **Pipeline**：批量命令用 Pipeline 减少 RTT
- **避免大 Key**：单个 value 不超过 1 MB
- **避免热 Key**：高 QPS key 考虑在客户端做本地缓存

### 12.4 性能基线参考

参考 [测试文档 - 性能基线](./TEST.md#82-性能基线参考值)。

---

## 13. 升级与回滚

### 13.1 升级流程

```bash
# 1. 停止服务
sudo systemctl stop concurrentcache

# 2. 备份当前二进制和配置
cp /opt/concurrentcache/bin/concurrentcache-server /backup/cc-server.$(date +%Y%m%d)
cp /opt/concurrentcache/conf/concurrentcache.conf /backup/cc.conf.$(date +%Y%m%d)

# 3. 部署新二进制
cp /path/to/new/concurrentcache-server /opt/concurrentcache/bin/
chown ccuser:ccuser /opt/concurrentcache/bin/concurrentcache-server

# 4. 启动
sudo systemctl start concurrentcache
sudo systemctl status concurrentcache

# 5. 验证
redis-cli -p 6379 PING
redis-cli -p 6379 INFO server | grep version
```

### 13.2 Docker 升级

```bash
# 1. 拉取新镜像
docker pull ghcr.io/dingziming/concurrentcache:new-tag

# 2. 滚动重启
docker stop concurrentcache
docker rm concurrentcache
docker run -d --name concurrentcache ... ghcr.io/dingziming/concurrentcache:new-tag

# 3. 验证
docker exec concurrentcache redis-cli -p 6379 PING
```

### 13.3 回滚

```bash
# 恢复旧二进制
sudo systemctl stop concurrentcache
cp /backup/cc-server.20260601 /opt/concurrentcache/bin/concurrentcache-server
sudo systemctl start concurrentcache
```

**注意**：跨大版本升级可能涉及 RDB 文件格式变更，需先用新版本试运行，确认无数据兼容问题再正式升级。

---

## 14. 故障 Runbook

### 14.1 服务无法启动

| 症状 | 排查步骤 |
|------|---------|
| 端口被占用 | `sudo lsof -i :6379` 杀掉占用进程或修改 `port` |
| 配置文件语法错 | 人工检查 `concurrentcache.conf` |
| 权限不足 | `chmod` / `chown` 数据目录 |
| zlib 缺失 | `apt install zlib1g` |

### 14.2 PING 无响应

```bash
# 1. 检查进程是否存活
ps aux | grep concurrentcache

# 2. 检查端口监听
ss -tlnp | grep 6379

# 3. 检查防火墙
sudo iptables -L -n | grep 6379
# 或
sudo ufw status

# 4. 查看最近日志
journalctl -u concurrentcache --since "5 minutes ago"
```

### 14.3 内存占用过高

```bash
# 查看 key 数量
redis-cli -p 6379 DBSIZE

# 找出大 key（需自己实现，可遍历 INFO）
# 或启用 max_entries 触发主动淘汰

# 紧急清理（慎用！）
redis-cli -p 6379 FLUSHDB
```

### 14.4 RDB 保存失败

```bash
# 查看上次保存状态
redis-cli -p 6379 INFO persistence | grep last_bgsave

# 检查磁盘空间
df -h /opt/concurrentcache/data

# 手动触发看错误信息
redis-cli -p 6379 BGSAVE
# 然后查看日志
```

### 14.5 主从复制中断

```bash
# 在从节点查看状态
redis-cli -p 6379 INFO replication

# 重点关注
# master_link_status:up   ← 正常应为 up
# master_last_io_seconds_ago:<秒数>
# master_sync_in_progress:1
```

如果 `master_link_status: down`，检查：

1. 主从网络连通性
2. 主节点是否启用了持久化（部分版本要求）
3. 防火墙是否放行端口

### 14.6 集群脑裂

**症状**：网络分区后出现两个"主"。

**应对**：

1. 设置 `cluster_node_timeout` 合理（默认 15000ms）
2. 设置 `cluster_require_full_coverage = true` 防止部分槽写入
3. 网络恢复后会自动合并

---

## 15. 安全加固

### 15.1 网络隔离

```bash
# 仅监听内网
port = 6379
# 服务端无 bind 配置时默认 0.0.0.0，外部可达
# 建议在防火墙层做限制
sudo iptables -A INPUT -p tcp --dport 6379 -s 10.0.0.0/8 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 6379 -j DROP
```

### 15.2 禁用危险命令（生产建议）

当前版本无 ACL。可在网络层代理（如 twemproxy、codis）做白名单。

`FLUSHDB`、`FLUSHALL`、`DEBUG` 等命令**慎用**，建议通过代理层禁用。

### 15.3 文件系统权限

```bash
# 数据目录仅 ccuser 可读写
chmod 700 /opt/concurrentcache/data
chown ccuser:ccuser /opt/concurrentcache/data
```

### 15.4 日志脱敏

避免在客户端命令日志中打印 value（特别是含敏感信息的场景）。服务日志默认不打印 value。

### 15.5 升级与补丁

- 关注 [GitHub Releases](https://github.com/dingziming/ConcurrentCache/releases) 安全公告
- 定期拉取最新镜像：`docker pull ghcr.io/dingziming/concurrentcache:latest`

---

## 16. 另见

### 16.1 相关文档

- [项目 README](../../README.md) — 项目总览
- [API 参考](./API.md) — 命令协议层
- [测试文档](./TEST.md) — 测试体系
- [架构总览](../developing/Architecture.md) — 系统架构

### 16.2 外部资源

- [Redis 官方文档](https://redis.io/documentation) — RESP 协议参考
- [Docker 官方文档](https://docs.docker.com/) — 容器化部署
- [systemd 服务配置](https://www.freedesktop.org/software/systemd/man/systemd.service.html) — 进程管理
- [Linux 高性能服务器编程](https://book.douban.com/subject/24772279/) — 网络调优参考

### 16.3 部署相关源码

- 服务入口：`main.cpp`
- 配置加载：`src/base/config.cpp`
- 网络层：`src/network/`
- 集群节点：`src/cluster/cluster_server.cpp`
- 持久化：`src/persistence/rdb_scheduler.cpp`

---

**部署完成后的快速自检清单**：

- [ ] `redis-cli PING` 返回 `PONG`
- [ ] `INFO server` 显示正确版本
- [ ] `DBSIZE` 与预期一致
- [ ] `CONFIG GET port` 与配置一致
- [ ] 日志无 ERROR 级条目
- [ ] 文件描述符上限 ≥ 65535
- [ ] RDB 备份目录可写
- [ ] 防火墙规则已生效
