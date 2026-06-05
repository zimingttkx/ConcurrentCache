# ConcurrentCache 文档中心

> 📚 **这是 `docs/` 目录的入口页**。所有文档按"快速上手 → 系统设计 → API → 测试 → 部署 → 历史档案"顺序组织。
> 25 份文档，全在这里。点哪个跳哪个，不迷路。

---

## 🎯 我是新来的，我该从哪里开始？

| 你的身份 | 推荐路线 | 大约耗时 |
|---------|---------|---------|
| **🆕 新加入项目** | ① → ② → ③ → ④ → ⑤ | 30 分钟 |
| **💻 后端开发** | ① → ③ → ⑥ → ⑦ | 20 分钟 |
| **🏗️ 系统架构师** | ① → ② → ⑥ → ⑦ | 25 分钟 |
| **🚀 运维 / SRE** | ① → ⑤ → ⑦ | 15 分钟 |
| **🧪 测试工程师** | ① → ④ → ⑦ | 15 分钟 |

| # | 必读 | 链接 | 一句话说明 |
|---|------|------|-----------|
| ① | **项目总览** | [`README.md`](../../README.md) | 5 分钟了解项目是什么、能做什么 |
| ② | **架构演进 V1→V4** | [`developing/Architecture.md`](developing/Architecture.md) | 理解项目从单线程到集群的演进思路 |
| ③ | **当前架构详细设计** | [`developing/CurrentProject_Detailed/01_概述与设计理念.md`](developing/CurrentProject_Detailed/01_概述与设计理念.md) | 6 章带你看完 6 个层的设计 |
| ④ | **API 命令手册** | [`api/API.md`](api/API.md) | 所有支持的命令、参数、示例 |
| ⑤ | **部署手册** | [`deploy/DEPLOY.md`](deploy/DEPLOY.md) | 源码 / Docker / 集群 / 备份 |
| ⑥ | **设计专题** | [`architecture/Architecture_V4.md`](architecture/Architecture_V4.md) | 当前版本（V4）完整架构图 |
| ⑦ | **测试体系** | [`test/TEST.md`](test/TEST.md) | 12 个 C++ 测试套件 + 8 个 E2E 脚本 |

---

## 📂 文档分类总览

```
docs/
├── 📘 README.md                 ← 你正在看这个
│
├── 🌟 核心文档（先看这里）  ───────────────
│   ├── api/API.md              ④ API 命令手册
│   ├── test/TEST.md            ⑦ 测试体系
│   └── deploy/DEPLOY.md        ⑤ 部署手册
│
├── 🏗️ 架构设计（按版本）  ─────────────────
│   ├── developing/
│   │   ├── Architecture.md     ② V1→V4 演进总览
│   │   ├── Roadmap.md          路线图汇总
│   │   ├── Dev.md              开发日志
│   │   └── CurrentProject_Detailed/
│   │       ├── 01_概述与设计理念.md     ③a 项目概述
│   │       ├── 02_应用层.md             ③b Command 模式
│   │       ├── 03_协议层.md             ③c RESP 协议
│   │       ├── 04_存储层.md             ③d 64 分片哈希
│   │       ├── 05_网络层.md             ③e MainSubReactor
│   │       └── 06_基础设施层.md         ③f Logger/Config/Signal
│   │
│   ├── architecture/
│   │   ├── Architecture_V2.md  V2 基础架构（MainSubReactor + 内存池）
│   │   ├── Architecture_V3.md  V3 增强架构（5 种数据类型 + RDB）
│   │   ├── Architecture_V4.md  ⑥ V4 分布式架构（集群 + 复制）
│   │   ├── Roadmap_V2.md      V2 排期
│   │   ├── Roadmap_V3.md      V3 排期
│   │   └── Roadmap_V4.md      V4 排期
│   │
│   └── development/
│       ├── README.md          迭代蓝图索引
│       ├── Checklist.md       V1-V4 验收清单
│       ├── Version1.md        V1 骨架版本
│       ├── Version2.md        V2 基础版本
│       ├── Version3.md        V3 增强版本
│       └── Version4.md        V4 分布式版本
│
└── 💭 杂项（按需阅读）  ───────────────────
    └── MyDevQuestions/
        └── DevQuestions.md    C++/内存/线程/网络 问答合集
```

---

## 🌟 核心文档（3 份，**最常查阅**）

### 📘 [API 命令手册 →](api/API.md)
**面向客户端开发者**。列出全部 Redis 兼容命令，每个命令含：语法、参数、返回值、多语言示例、错误场景。

> **何时看**：你要写客户端代码、查命令格式、调 bug。
> **快速跳转**：[快速开始](api/API.md#2-快速开始) · [字符串命令](api/API.md#52-字符串-string) · [列表命令](api/API.md#54-列表-list) · [错误码](api/API.md#6-错误参考)

---

### 🧪 [测试体系 →](test/TEST.md)
**面向测试与开发**。12 个 C++ 测试套件 + 8 个 Python E2E 脚本，含编译、运行、CI 集成、故障排查。

> **何时看**：你要加新功能写测试、改代码想跑回归、性能压测。
> **快速跳转**：[测试套件清单](test/TEST.md#2-测试体系结构) · [运行测试](test/TEST.md#3-快速运行) · [E2E 脚本](test/TEST.md#6-端到端测试) · [CI 集成](test/TEST.md#7-ci-集成)

---

### 🚀 [部署手册 →](deploy/DEPLOY.md)
**面向运维与部署**。源码编译、Docker、Docker Compose、集群部署、配置详解、备份恢复、故障 Runbook。

> **何时看**：你要上线、扩容、查问题、调性能、备份数据。
> **快速跳转**：[Docker 部署](deploy/DEPLOY.md#5-docker-部署) · [集群部署](deploy/DEPLOY.md#9-集群部署) · [配置详解](deploy/DEPLOY.md#7-配置文件详解) · [故障 Runbook](deploy/DEPLOY.md#14-故障-runbook)

---

## 🏗️ 架构设计（按"先总后分"顺序阅读）

### 第一步：先看总览

#### 🔭 [主架构文档（V1→V4 演进）→](developing/Architecture.md)
**从单线程 Reactor 到分布式集群的演进全过程**。理解为什么这么设计、每一步解决了什么问题。

#### 🛣️ [路线图汇总 →](developing/Roadmap.md)
**已完成 vs 待办模块清单**。一眼看清项目当前进度。

#### 📓 [开发日志 →](developing/Dev.md)
**关键节点的决策记录**。类似博客，串起来看有故事感。

---

### 第二步：逐层深入（推荐 6 章连读）

`developing/CurrentProject_Detailed/` 是当前架构的 6 章详细设计文档，按数据流方向组织：

| # | 章节 | 链接 | 核心问题 |
|---|------|------|---------|
| 1 | 概述与设计理念 | [📄](developing/CurrentProject_Detailed/01_概述与设计理念.md) | 项目是什么？设计目标？关键约束？ |
| 2 | 应用层 | [📄](developing/CurrentProject_Detailed/02_应用层.md) | 命令模式怎么落地？CommandFactory 怎么工作？ |
| 3 | 协议层 | [📄](developing/CurrentProject_Detailed/03_协议层.md) | RESP 协议怎么解析？怎么编解码？ |
| 4 | 存储层 | [📄](developing/CurrentProject_Detailed/04_存储层.md) | 64 分片哈希表怎么分锁？5 种类型怎么存？ |
| 5 | 网络层 | [📄](developing/CurrentProject_Detailed/05_网络层.md) | epoll 怎么用？MainSubReactor 怎么分工？ |
| 6 | 基础设施层 | [📄](developing/CurrentProject_Detailed/06_基础设施层.md) | 日志、配置、信号、锁、格式化 |

> 💡 **阅读建议**：从 01 顺序读，每个 5-10 分钟。如果赶时间，至少读完 01 + 04 + 05 三个核心层。

---

### 第三步：版本专题（按需看）

`architecture/` 目录是各版本的专题架构图：

| 版本 | 架构图 | 路线图 | 关键新增 |
|------|-------|-------|---------|
| **V2** | [📐 Architecture_V2](architecture/Architecture_V2.md) | [📅 Roadmap_V2](architecture/Roadmap_V2.md) | MainSubReactor + 三级内存池 + 64 分片 |
| **V3** | [📐 Architecture_V3](architecture/Architecture_V3.md) | [📅 Roadmap_V3](architecture/Roadmap_V3.md) | 5 种数据类型 + RDB 持久化 |
| **V4** | [📐 Architecture_V4](architecture/Architecture_V4.md) | [📅 Roadmap_V4](architecture/Roadmap_V4.md) | 16384 哈希槽 + Gossip + 主从复制 |

> V4 是当前最新版本，V2/V3 是历史已实现版本。

---

### 第四步：版本实现记录

`development/` 是各版本的实际实现记录（与 `architecture/` 互补——一个是设计图，一个是施工日志）：

| 文档 | 链接 | 说明 |
|------|------|------|
| 迭代蓝图 | [📄 README](development/README.md) | 4 版本演进索引页 |
| 验收清单 | [📄 Checklist](development/Checklist.md) | V1-V4 全部验收项，可勾选 |
| V1 实现 | [📄 Version1](development/Version1.md) | 单 Reactor 骨架 |
| V2 实现 | [📄 Version2](development/Version2.md) | 基础版本 |
| V3 实现 | [📄 Version3](development/Version3.md) | 增强版本 |
| V4 实现 | [📄 Version4](development/Version4.md) | 分布式版本 |

---

## 💭 杂项（按需阅读）

#### 🤔 [C++/内存/线程/网络 问答合集 →](MyDevQuestions/DevQuestions.md)
**Q&A 形式的开发笔记**。记录开发过程中遇到的具体问题、坑、解决方案。
适合：遇到具体技术问题时搜索关键词查答案。

---

## 🔍 快速检索

按"我想做什么"反查文档：

| 我想... | 看这个 |
|---------|--------|
| 🚀 5 分钟了解项目 | [README.md](../../README.md) |
| 📚 了解整体架构 | [developing/Architecture.md](developing/Architecture.md) |
| 🔍 深入某个层的实现 | [developing/CurrentProject_Detailed/01-06](developing/CurrentProject_Detailed/01_概述与设计理念.md) |
| 💻 查某个命令怎么用 | [api/API.md](api/API.md) |
| 🧪 跑测试 / 写测试 | [test/TEST.md](test/TEST.md) |
| 🐳 部署到服务器 | [deploy/DEPLOY.md](deploy/DEPLOY.md) |
| ⚙️ 改配置参数 | [deploy/DEPLOY.md#7-配置文件详解](deploy/DEPLOY.md#7-配置文件详解) |
| 🛡️ 集群扩容 / 加节点 | [deploy/DEPLOY.md#9-集群部署](deploy/DEPLOY.md#9-集群部署) |
| 🆘 服务挂了 | [deploy/DEPLOY.md#14-故障-runbook](deploy/DEPLOY.md#14-故障-runbook) |
| 📊 性能调优 | [deploy/DEPLOY.md#12-性能调优](deploy/DEPLOY.md#12-性能调优) |
| 💾 备份与恢复 | [deploy/DEPLOY.md#11-备份与恢复](deploy/DEPLOY.md#11-备份与恢复) |
| 🕰️ 查历史决策 | [developing/Dev.md](developing/Dev.md) |
| ❓ C++ 具体技术问题 | [MyDevQuestions/DevQuestions.md](MyDevQuestions/DevQuestions.md) |

---

## 📊 文档统计

| 分类 | 文档数 |
|------|-------|
| 核心文档（API / Test / Deploy） | 3 |
| 架构设计（developing + architecture） | 14 |
| 版本实现记录（development） | 6 |
| 杂项（MyDevQuestions） | 1 |
| 入口页（本文件） | 1 |
| **合计** | **25** |

| 文档类型 | 数量 | 完整度 |
|---------|------|--------|
| 系统设计 | 15 | ✅ 充分 |
| API | 1 | ✅ 已建 |
| 测试 | 1 | ✅ 已建 |
| 部署 | 1 | ✅ 已建 |
| 需求 | 0 | ❌ 缺失 |
| 用户手册 | 0 | ❌ 缺失 |

---

## 🛠️ 维护约定

> 任何代码变更都需同步更新对应章节。

| 变更类型 | 同步更新 |
|---------|---------|
| 新增 / 修改 / 删除命令 | `api/API.md` |
| 新增 / 修改 / 删除测试 | `test/TEST.md` |
| 新增 / 修改 / 删除配置项 | `deploy/DEPLOY.md` 第 7 章 |
| 架构 / 模块调整 | `developing/CurrentProject_Detailed/01-06` 对应章节 + `developing/Architecture.md` |
| 新建迭代 / 完成里程碑 | `development/Checklist.md` + `development/Version*.md` |
| 重大决策 | `developing/Dev.md` 加一条 |

---

**✅ 推荐开始阅读**：[📘 API 命令手册](api/API.md) → [🏗️ 项目总览架构](developing/Architecture.md) → [🚀 部署手册](deploy/DEPLOY.md)
