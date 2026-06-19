# ConcurrentCache 文档中心

> 项目所有技术文档的导航入口。**新读者从这里开始**。

| 文档状态 | 维护者 | 最后更新 | 适用版本 |
|---------|--------|---------|---------|
| Active | ConcurrentCache Team | 2026-06 | V3.x |

---

## 1. 文档结构

```text
docs/
├── README.md                   ← 本文件（导航入口）
│
├── architecture/               ← 架构设计（按层拆分）
│   ├── overview.md             系统分层、组件依赖、请求时序、性能数据
│   ├── network.md              MainReactor / SubReactor / epoll
│   ├── storage.md              64 分片哈希 + 5 数据类型 + 过期 + ARU
│   ├── memory-pool.md          ThreadCache / CentralCache / PageCache（当前未启用）
│   ├── persistence.md          RDB 快照与加载
│   └── cluster.md              16384 槽 + Gossip + 主从复制
│
├── api.md                      ← API 命令手册（44 个命令）
├── testing.md                  ← 测试体系（12 C++ + 11 Python）
└── deployment.md               ← 部署与运维（含性能基准数据）
```

## 2. 读者路径

| 身份 | 推荐阅读顺序 | 预估时长 |
|------|------------|---------|
| **新加入项目** | [项目 README](../../README.md) → `architecture/overview.md` → `architecture/network.md` → `architecture/storage.md` → `api.md` | 60 min |
| **后端开发** | `architecture/overview.md` → `architecture/storage.md` → `api.md` → `testing.md` | 40 min |
| **架构师** | `architecture/overview.md` → `architecture/cluster.md` → `architecture/persistence.md` | 30 min |
| **运维 / SRE** | `deployment.md` → `architecture/overview.md` → `api.md` | 25 min |
| **测试工程师** | `testing.md` → `api.md` | 20 min |

## 3. 主题索引

### 3.1 架构（`docs/architecture/`）

| 文档 | 内容 | 何时看 |
|------|------|-------|
| [`overview.md`](architecture/overview.md) | 7 层组件图、模块依赖、请求时序、关键不变量、性能数据 | **第一份必读** |
| [`network.md`](architecture/network.md) | `MainReactor` 端口监听、`SubReactorPool` 轮询、`EventLoop` epoll LT、`Connection` 缓冲区 | 改网络层、调连接性能 |
| [`storage.md`](architecture/storage.md) | `GlobalStorage` 64 分片 + `std::shared_mutex`、`ExpireDict` 过期字典、`ExpirationChecker` 定期清理、ARU 淘汰 | 改存储、调并发瓶颈 |
| [`memory-pool.md`](architecture/memory-pool.md) | `SizeClass` 29 级、`ThreadCache` 无锁分配、`CentralCache` 细粒度锁、`PageCache` Span 管理 | 改内存池、分析碎片 |
| [`persistence.md`](architecture/persistence.md) | RDB 魔数 `CCRD`、5 类型序列化、`RdbScheduler` 周期+阈值触发、优雅退出保存 | 改持久化、查数据丢失 |
| [`cluster.md`](architecture/cluster.md) | `ClusterServer` 单例、16384 槽 CRC16、`ClusterGossip` Ping/Pong/Meet/Fail、`ReplicationMgr` 10MB 缓冲、客观下线+投票 | 部署集群、排查主从 |

### 3.2 API

| 文档 | 内容 | 何时看 |
|------|------|-------|
| [`api.md`](api.md) | 全部 44 个命令语法、参数、RESP 响应、错误码、客户端示例、不支持特性清单 | 写客户端代码、查命令格式 |

### 3.3 测试

| 文档 | 内容 | 何时看 |
|------|------|-------|
| [`testing.md`](testing.md) | 12 个 C++ 测试套件、11 个 Python E2E 脚本（含 Redis 对比测试）、Sanitizer 用法、CI 流程 | 加新功能写测试、跑回归 |

### 3.4 部署与运维

| 文档 | 内容 | 何时看 |
|------|------|-------|
| [`deployment.md`](deployment.md) | 编译选项、Docker 部署、systemd、配置项、故障排查、性能基准数据 | 上线、调配置、查问题 |

## 4. 文档维护规则

| 变更类型 | 同步更新 |
|---------|---------|
| 新增 / 修改 / 删除命令 | [`api.md`](api.md) |
| 新增 / 修改 / 删除测试 | [`testing.md`](testing.md) |
| 新增 / 修改 / 删除配置项 | [`deployment.md`](deployment.md) § 4 配置项 |
| 模块 / 架构调整 | 对应 `architecture/*.md` + `architecture/overview.md` 模块依赖图 |
| 集群协议 / 槽位 / 复制变更 | `architecture/cluster.md` |
| 性能基准更新 | `deployment.md` § 11 + `architecture/overview.md` § 7 |

> 任何文档与代码不一致时，**以代码为准**，并提 PR 修正文档。

## 5. 反馈

- 文档错误 / 缺漏：提交 Issue，标签 `docs`
- 内容建议：直接 PR 修改对应 `.md`