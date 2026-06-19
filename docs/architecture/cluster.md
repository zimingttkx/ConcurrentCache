# 集群架构

> **范围**：16384 哈希槽分片、Gossip 协议、主从复制、故障检测与故障转移、ClusterBus 节点通信。
> **源码**：`src/cluster/`
> **启用条件**：`conf/concurrentcache.conf` 设置 `cluster_enabled = true`
> **前置阅读**：[架构总览](./overview.md) · [网络层](./network.md)

## 1. 设计目标

| 目标 | 手段 |
|------|------|
| 横向扩展 | 16384 哈希槽（CRC16(key) % 16384） |
| 节点发现 | Gossip（PING/PONG/MEET） |
| 故障自动恢复 | 客观下线 + 投票 + 故障转移 |
| 数据高可用 | 主从复制 + 10MB 复制缓冲 |
| 客户端透明 | MOVED / ASK 重定向 |
| 节点通信隔离 | ClusterBus 监听 `port + 10000` |

## 2. 核心组件

```mermaid
flowchart TB
    CS[ClusterServer<br/>单例]
    CS --> ST[ClusterState<br/>节点表 + 槽表 + 复制关系]
    CS --> CN[ClusterNode<br/>flags + slots + 复制状态]
    CS --> CC[ClusterConnection<br/>连接管理]
    CS --> CB[ClusterBus<br/>port+10000 TCP 服务]
    CS --> CG[ClusterGossip<br/>PING/PONG/MEET/FAIL]
    CS --> RM[ReplicationMgr<br/>10MB 复制缓冲 + PSYNC]

    CB --> CL[ClusterLink]
    CG --> ST
    RM --> ST
    CC --> CS
```

### 2.1 `ClusterServer`（`src/cluster/cluster_server.{h,cpp}`）

| 职责 | 接口 |
|------|------|
| 节点生命周期 | `init()` / `start()` / `stop()` |
| 槽路由 | `keyToSlot(key)` / `getNodeByKey(key)` / `getNodeBySlot(slot)` |
| 重定向检查 | `checkRedirect(key)`（返回 MOVED/ASK RESP 字符串） |
| 主从关系 | `setReplicaOf(master_name)` / `clearReplicaOf()` |
| 故障检测 | `handleNodeTimeout(name)` / `markNodeAsFail(name)` |
| 故障转移 | `startFailover(master_name)` / `executeFailover()` |
| 投票 | `handleFailoverAuthRequest(...)` / `handleFailoverAuthAck(...)` |

**关键成员**：

| 成员 | 说明 |
|------|------|
| `enabled_` | 是否启用集群（读 `cluster_enabled` 配置） |
| `my_node_` | 本节点（`shared_ptr<ClusterNode>`） |
| `state_` | 集群状态（节点表 + 槽表 + 复制关系） |
| `connection_` | 连接管理器 |
| `gossip_` | Gossip 协议 |
| `cluster_bus_` | 集群总线（监听 `port + 10000`） |
| `running_` | 运行标志 |

### 2.2 `ClusterState`（`src/cluster/cluster_state.{h,cpp}`）

| 成员 | 类型 | 锁 |
|------|------|---|
| `nodes_` | `unordered_map<string, shared_ptr<ClusterNode>>` | `std::shared_mutex` |
| `slots_` | `unordered_map<int, shared_ptr<ClusterNode>>`（16384 项） | `slots_mutex_` |
| `migrating_slots_` | `unordered_map<int, SlotMigrationInfo>` | `migration_mutex_` |
| `importing_slots_` | `unordered_map<int, SlotMigrationInfo>` | `migration_mutex_` |
| `replicas_` | `unordered_map<string, vector<shared_ptr<ClusterNode>>>` | `replicas_mutex_` |
| `pfailing_reports_` | `unordered_map<string, unordered_set<string>>` | `pfail_mutex_` |

**为什么用 `shared_ptr` 存槽？** 同一节点可能被多个槽引用，存指针避免 `unordered_map<int, ClusterNode>` 时的二次查找。

### 2.3 `ClusterNode`（`src/cluster/cluster_node.{h,cpp}`）

| 字段 | 类型 | 说明 |
|------|------|------|
| `info_` | `NodeInfo` | 基础信息（name/ip/port/role/flags/心跳时间） |
| `slots_` | `vector<int>` + `mutex` | 该节点负责的槽 |
| `master_node_` | `weak_ptr<ClusterNode>` | 副本的主节点 |
| `replication_state_` | `ReplicationState` | 复制阶段 |
| `failure_count_` | `atomic<int>` | PING 失败计数 |
| `failover_state_` | `atomic<int>` | 故障转移状态 |
| `votes_` | `vector<VoteInfo>` + `votes_mutex_` | 收到的投票 |

**`NodeFlags`**（位掩码）：

| Flag | 含义 |
|------|------|
| `kFail` | 客观下线（已通过投票确认） |
| `kPfail` | 主观下线（单个节点认为该节点不可达） |
| `kHandshake` | 正在握手 |
| `kNoAddress` | 地址未知 |

## 3. 哈希槽分片

```mermaid
flowchart LR
    K[key] --> CRC[CRC16]
    CRC --> MOD[mod 16384]
    MOD --> SLOT[slot 0-16383]
    SLOT --> NODE[ClusterNode]
```

**算法**：

```cpp
int ClusterServer::keyToSlot(const std::string& key) const {
    // 简化版：CRC16(key) % 16384
    // Redis 实际用 CRC16-CCITT，多项式 0x1021
    // 当前实现见 cluster_server.cpp
    return state_.getNodeForSlot(slot) ? ... : 0;
}
```

**为什么是 16384？** Redis Cluster 设计选择。2^14 = 16384 是权衡后的结果：

- 节点数 ≤ 1000 时每节点约 16 个槽，迁移粒度合适
- 心跳消息中用 2KB 位图（16384 bits = 2048 bytes）即可表示完整槽分配

**槽迁移状态**（`SlotMigrationInfo`）：

```cpp
struct SlotMigrationInfo {
    int slot = -1;
    SlotMigrationStatus status = SlotMigrationStatus::kNone;
    std::string source_node;   // 迁出节点
    std::string target_node;   // 迁入节点
};
```

**MOVED vs ASK 重定向**：

| 状态 | 含义 | 响应 |
|------|------|------|
| 槽已迁出 | 客户端应长期重定向 | `-MOVED slot ip:port\r\n` |
| 槽正在迁入 | 客户端应本次请求重定向 | `-ASK slot ip:port\r\n` |

## 4. Gossip 协议

### 4.1 消息类型（`GossipType`）

| 类型 | 值 | 方向 | 用途 |
|------|---|------|------|
| `kPing` | 1 | → 邻居 | 心跳 |
| `kPong` | 2 | → 邻居 | 心跳响应 |
| `kMeet` | 3 | → 新节点 | 加入集群 |
| `kFail` | 4 | 广播 | 节点下线广播 |
| `kFailoverAuthReq` | 5 | 广播 | 故障转移投票请求 |
| `kFailoverAuthAck` | 6 | 广播 | 投票确认 |
| `kPush` | 7 | → 邻居 | 推送本节点信息 |
| `kPull` | 8 | → 邻居 | 拉取本节点信息 |

### 4.2 心跳流程

```mermaid
sequenceDiagram
    participant A as 节点 A
    participant B as 节点 B
    participant C as 节点 C

    loop 每 1s（gossip 间隔）
        A->>B: PING (携带 A 视角的集群视图)
        B-->>A: PONG (携带 B 视角)
        A->>A: 更新本地视图（merge 节点信息）
    end

    A->>B: PING
    Note over A,B: 1000ms 内未收到 PONG<br/>failure_count++

    A->>A: failure_count >= 3<br/>标记 B 为 PFAIL
    A->>A: 多数节点报告 PFAIL<br/>升级为 FAIL
```

### 4.3 `ClusterGossip`（`src/cluster/cluster_gossip.{h,cpp}`）

| 关键方法 | 作用 |
|---------|------|
| `init(state)` | 初始化（持有 state 指针） |
| `build_ping/pong/meet_msg()` | 构造 Gossip 消息 |
| `handle_ping/pong/meet/fail/...` | 处理收到的消息 |
| `send_gossip(msg)` | 发送给随机邻居 |
| `broadcast_fail(name)` | 广播下线消息 |
| `get_random_nodes(count)` | 选 gossip 目标 |
| `push_node_info(node)` / `pull_node_info()` | 配置传播 |

## 5. 主从复制

### 5.1 复制状态机

```mermaid
stateDiagram-v2
    [*] --> kNone: 初始
    kNone --> kConnect: 收到 REPLICAOF
    kConnect --> kHandshake: TCP 建立
    kHandshake --> kSync: 发送 PSYNC
    kSync --> kSendingRdb: 主发 RDB
    kSendingRdb --> kSendingkv: 增量命令
    kSendingkv --> kConnected: offset 追上
    kConnected --> kConnected: 持续复制
```

### 5.2 `ReplicationMgr`（`src/cluster/replication_mgr.{h,cpp}`）

| 成员 | 类型 | 说明 |
|------|------|------|
| `replicas_` | `unordered_map<string, shared_ptr<ReplicaInfo>>` | 副本列表（主端维护） |
| `repl_buffer_` | `vector<ReplicationBufferEntry>` | 10MB 环形缓冲 |
| `master_repl_offset_` | `atomic<int64_t>` | 主节点当前偏移量 |
| `sync_state_` | `atomic<SyncState>` | 副本端同步阶段 |
| `master_ip_/port_/runid_` | string/int | 副本端记录的主节点 |

**`ReplicaInfo`**：

```cpp
struct ReplicaInfo {
    std::string name;
    std::string ip;
    int port;
    int64_t repl_offset;
    int64_t last_ack_time;
    SyncState sync_state;
    std::shared_ptr<ClusterNode> node;
};
```

**环形缓冲**：

```cpp
static constexpr size_t kReplicationBufferSize = 10 * 1024 * 1024;  // 10MB
std::vector<ReplicationBufferEntry> repl_buffer_;
```

**复制流程**（简化）：

```text
主端：
  1. 收到写命令 → replicate_command(cmd) 写 repl_buffer_
  2. 后台线程遍历 replicas_，发送未确认的缓冲

副本端：
  1. handle_replication_command(cmd_line)
  2. 在本地执行相同命令
  3. 回复 REPLCONF ACK offset
```

### 5.3 复制协议命令

| 命令 | 方向 | 用途 |
|------|------|------|
| `PSYNC ? -1` | 副本 → 主 | 全量同步请求 |
| `PSYNC <runid> <offset>` | 副本 → 主 | 增量同步请求 |
| `+FULLRESYNC <runid> <offset>` | 主 → 副本 | 全量同步响应（含 RDB 跟随） |
| `+CONTINUE` | 主 → 副本 | 增量同步成功 |
| `REPLCONF listening-port <port>` | 副本 → 主 | 注册端口 |
| `REPLCONF ACK <offset>` | 副本 → 主 | 确认偏移 |

## 6. 故障检测与转移

### 6.1 客观下线算法

```mermaid
flowchart TB
    A[PING 超时] --> B[failure_count++]
    B --> C{count >= 3?}
    C -->|Yes| D[标记 PFAIL<br/>PFAIL 报告广播]
    C -->|No| A
    D --> E[checkFailQuorum<br/>收集 PFAIL 报告]
    E --> F{多数报告?}
    F -->|Yes| G[标记 FAIL<br/>广播 FAIL 消息]
    F -->|No| A

    G --> H[从节点检测到主节点 FAIL]
    H --> I{replica_priority<br/>最高?}
    I -->|Yes| J[startFailover]
    I -->|No| K[等待其他从节点]
    J --> L[广播 FailoverAuthReq]
    L --> M[等待 ack 超时]
    M --> N{收到多数 ACK?}
    N -->|Yes| O[completeFailover<br/>本节点晋升主]
    N -->|No| P[FailoverTimeout]
    O --> Q[broadcastFailoverUpdate]
```

### 6.2 关键不变量

- **故障检测参数**（`FailureDetectionConfig`）：

  ```cpp
  int64_t node_timeout_ms = 5000;
  int max_ping_failures = 3;
  int quorum = 2;  // 多数（> N/2）
  ```

- **故障转移参数**（`FailoverConfig`）：

  ```cpp
  int64_t failover_timeout_ms = 30000;
  int64_t auth_timeout_ms = 5000;
  int replica_priority = 100;
  ```

- 同一 epoch 内只能有一个从节点发起投票请求成功（避免脑裂）

## 7. ClusterBus 节点通信

| 项 | 值 |
|----|---|
| 监听端口 | `server_port + 10000`（默认 16379 + 10000 = 26379） |
| 协议 | TCP + 自定义二进制帧 |
| 用途 | 节点间消息转发（PING/PONG/FAIL/复制命令） |

**为什么 +10000？** 与 Redis Cluster 约定一致，方便客户端识别集群端口。

**`ClusterLink`**：单条节点间 TCP 连接。负责：

- 消息编解码
- 异步发送
- 心跳（与 Gossip 协同）
- 断开重连

## 8. 客户端重定向

`ClusterServer::checkRedirect(key)` 在命令执行前调用：

```cpp
std::string ClusterServer::checkRedirect(const std::string& key) const {
    int slot = keyToSlot(key);
    auto target = state_.getNodeForSlot(slot);
    if (target->getName() == my_node_name) {
        return "";  // 本节点负责，直接执行
    }
    if (state_.isSlotMigrating(slot)) {
        return RespEncoder::encode_error("ASK ...");
    }
    return RespEncoder::encode_error("MOVED ...");
}
```

**客户端应做**：

- 收到 `MOVED`：更新本地槽映射表，永久重定向到目标节点
- 收到 `ASK`：仅本次请求重定向到目标节点（不更新映射）

## 9. 关键不变量

| 不变量 | 维护机制 |
|--------|---------|
| 槽数 = 16384 | CRC16 mod 16384 |
| 单实例 | `ClusterServer::instance()` Magic Static |
| 槽表与节点表一致 | 所有修改都加 `slots_mutex_` + `mutex_` |
| 同一时刻只有一个从节点晋升 | epoch 单调递增 + 投票多数 |
| 复制不丢命令 | 10MB 环形缓冲 + offset 确认 |
| 客户端最终能拿到数据 | MOVED 重定向 + 客户端缓存槽表 |
| 节点通信与客户端通信隔离 | `ClusterBus` 监听 `port + 10000` |

## 10. 配置

`conf/concurrentcache.conf`：

```ini
cluster_enabled = true
cluster_node_timeout = 5000
```

`CLUSTER MEET <ip> <port>` 动态加入集群（`cluster_cmd.cpp`）。

## 11. 性能与调优

| 现象 | 排查 / 调优 |
|------|------|
| 心跳消息过大 | 减少 `get_random_nodes` count |
| 频繁 FAIL 抖动 | 调大 `max_ping_failures` |
| 故障转移慢 | 调小 `failover_timeout_ms` |
| 复制延迟高 | 调大 `kReplicationBufferSize` |
| 槽分布不均 | 用 `CLUSTER REBALANCE` 重分配 |
| 脑裂 | 确认 `quorum > N/2` |

## 12. 关键源码位置

| 关注点 | 文件 |
|--------|------|
| 槽路由 | `src/cluster/cluster_server.cpp`（`keyToSlot/getNodeByKey`） |
| 节点管理 | `src/cluster/cluster_state.cpp`（`addNode/setNodeForSlot`） |
| 心跳 | `src/cluster/cluster_gossip.cpp`（`handle_ping/pong`） |
| 复制缓冲 | `src/cluster/replication_mgr.cpp`（`replicate_command/add_to_replication_buffer`） |
| 故障转移 | `src/cluster/cluster_server.cpp`（`startFailover/executeFailover`） |
| 节点通信 | `src/cluster/cluster_bus.cpp`（`handle_accept/create_link`） |
| 客户端命令 | `src/command/cluster_cmd.cpp`（`CLUSTER MEET/SLOTS/NODES`） |
| 复制协议 | `src/command/psync_cmd.cpp`（`PSYNC`） |

## 13. 另见

- [API § 集群命令](../api.md)
- [部署 § 集群部署](../deployment.md)
- [测试 § 集群测试](../testing.md)
