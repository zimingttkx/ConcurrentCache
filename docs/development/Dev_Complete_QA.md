# 第七章：常见问题与解决方案

## 7.1 开发问题

### 7.1.1 内存相关问题

#### 内存泄漏

**问题描述**：程序运行时间越长，内存占用越高，最终可能导致OOM。

**常见原因**：
- 内存分配后未释放
- 智能指针使用不当
- 循环引用导致对象无法释放
- 第三方库内存泄漏

**排查方法**：
```bash
# 使用valgrind检测内存泄漏
valgrind --leak-check=full ./concurrentcache

# 输出示例：
==12345== Memcheck, a memory error detector
==12345== HEAP SUMMARY:
==12345==   in use at exit: 0 bytes in 0 blocks
==12345==   total heap usage: 1,000,000 allocs, 1,000,000 frees, 0 bytes allocated
```

**解决方案**：
1. 使用智能指针（std::unique_ptr/std::shared_ptr）
2. 遵循RAII原则
3. 使用内存池管理内存
4. 定期检查内存使用情况

#### 内存碎片

**问题描述**：内存使用率不高，但无法分配大块内存。

**原因分析**：
```
内存碎片产生过程：
┌─────────────────────────────────────────────────────────────┐
│  初始状态：                                                  │
│  [已分配][空闲][已分配][已分配][空闲][已分配]               │
│                                                              │
│  分配新内存（需要3个连续块）：                              │
│  → 找不到连续的3个空闲块                                   │
│  → 即使总空闲空间足够，也无法分配                           │
└─────────────────────────────────────────────────────────────┘
```

**解决方案**：
1. 使用内存池预分配大块内存
2. 使用固定大小的Size Class
3. 定期整理内存碎片
4. 使用slab分配器

#### 野指针/悬挂指针

**问题描述**：访问已释放的内存。

**常见场景**：
```cpp
// 场景1：返回局部指针
char* get_string() {
    char buffer[100];
    return buffer;  // 错误：buffer在函数结束后被销毁
}

// 场景2：删除后继续使用
delete ptr;
cout << ptr->value;  // 错误：ptr已成为悬挂指针

// 场景3：迭代器失效
for (auto it = vec.begin(); it != vec.end(); ++it) {
    if (should_delete(*it)) {
        vec.erase(it);  // 错误：erase后it失效
    }
}
```

**解决方案**：
1. 使用智能指针代替原始指针
2. 使用RAII管理资源
3. 删除指针后置为nullptr
4. 使用安全的容器操作

---

### 7.1.2 多线程相关问题

#### 数据竞争

**问题描述**：多个线程同时访问共享资源，导致数据不一致。

**排查方法**：
```bash
# 使用ThreadSanitizer检测数据竞争
g++ -fsanitize=thread -g -O2 source.cpp -o concurrentcache
./concurrentcache

# 输出示例：
WARNING: ThreadSanitizer: data race at 0x7fff12345678
  Write of size 8 by thread 1:
    #0 HashTable::set() at hash_table.cpp:123
  Previous write of size 8 by thread 2:
    #1 HashTable::set() at hash_table.cpp:123
```

**解决方案**：
```cpp
// 错误：没有加锁
void set(const std::string& key, const std::string& value) {
    data_[key] = value;  // 数据竞争！
}

// 正确：使用锁保护
void set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = value;
}
```

#### 死锁

**问题描述**：两个或多个线程相互等待对方释放锁，导致程序卡死。

**常见场景**：
```cpp
// 场景1：锁顺序不一致
// 线程A：
lock_a.lock();
lock_b.lock();
// ...

// 线程B：
lock_b.lock();  // 线程B已持有lock_b
lock_a.lock();  // 等待线程A释放lock_a

// 线程A：
lock_a.lock();  // 等待线程B释放lock_b
lock_b.lock();  // 但线程B在等待lock_a！死锁！
```

**解决方案**：
1. 始终按固定顺序获取锁
2. 使用std::lock同时获取多把锁
3. 使用锁超时避免永久等待
4. 减少锁的粒度和持有时间

```cpp
// 解决方案：固定锁顺序
void set_a() {
    std::lock(lock_a_, lock_b_);  // 同时获取
    // ...
}

// 或者：始终先获取lock_a，再获取lock_b
void set_a() {
    std::lock_guard<std::mutex> lock_a(lock_a_);
    std::lock_guard<std::mutex> lock_b(lock_b_);
    // ...
}
```

#### 伪共享

**问题描述**：多个线程访问同一个缓存行中的不同数据，导致缓存行失效。

**问题分析**：
```
CPU缓存行（64字节）：
┌─────────────────────────────────────────────────────────────┐
│ [变量A: 8字节] [变量B: 8字节] [变量C: 8字节] ... [填充]   │
└─────────────────────────────────────────────────────────────┘

线程1修改变量A：
┌─────────────────────────────────────────────────────────────┐
│ [变量A: 已修改] [变量B: 未修改] [变量C: 未修改] ...        │
└─────────────────────────────────────────────────────────────┘
  ↓
CPU需要重新加载整个缓存行（包含B、C）

线程2修改变量B：
┌─────────────────────────────────────────────────────────────┐
│ [变量A: 未修改] [变量B: 已修改] [变量C: 未修改] ...        │
└─────────────────────────────────────────────────────────────┘
  ↓
CPU需要重新加载整个缓存行（包含A、C）

结果：虽然A和B是独立的变量，但修改A会导致B的缓存失效，反之亦然。
```

**解决方案**：
```cpp
struct alignas(64) Counter {  // 对齐到缓存行
    std::atomic<uint64_t> counter{0};
};

// 或者使用padding
struct Counter {
    std::atomic<uint64_t> counter{0};
    char padding[64 - sizeof(std::atomic<uint64_t>)];
};
```

---

### 7.1.3 网络相关问题

#### 粘包/半包

**问题描述**：接收到的数据不完整或包含多个包。

**原因分析**：
```
发送：
[数据1][\r\n][数据2][\r\n]

可能收到：
1. [数据1][\r\n][数据2]        → 正常
2. [数据1][\r]                 → 半包，需要等待\n
3. [数据1][\r\n][数据2][\r\n][数据3]  → 粘包，需要拆分
```

**解决方案**：
1. 使用换行符分隔
2. 使用长度前缀
3. 使用固定大小的缓冲区

```cpp
class Buffer {
private:
    std::vector<char> buffer_;
    size_t read_index_ = 0;
    size_t write_index_ = 0;

public:
    // 按行读取
    std::string read_line() {
        auto pos = find('\r\n');
        if (pos == std::string::npos) return "";
        std::string line(buffer_.begin() + read_index_,
                         buffer_.begin() + pos);
        read_index_ = pos + 2;
        return line;
    }
};
```

#### 连接泄漏

**问题描述**：连接未正确关闭，导致文件描述符泄漏。

**常见场景**：
```cpp
// 场景1：异常时未关闭连接
void handle_connection(int fd) {
    Connection conn(fd);
    if (some_error) {
        return;  // 连接未关闭！
    }
    conn.close();
}

// 场景2：过早关闭连接
void handle_connection(int fd) {
    Connection conn(fd);
    conn.send_response();
    conn.close();  // 响应可能还没发送完
}
```

**解决方案**：
1. 使用RAII管理连接生命周期
2. 确保所有退出路径都关闭连接
3. 使用智能指针管理连接

---

## 7.2 性能问题

### 7.2.1 CPU瓶颈

#### 热点代码

**问题描述**：少量代码占用了大量CPU时间。

**排查方法**：
```bash
# 使用perf分析CPU热点
perf record -g ./concurrentcache
perf report

# 输出示例：
Overhead  Command          Shared Object     Symbol
   45.23%  concurrentcache  libstdc++.so.6   std::string::replace
   23.45%  concurrentcache  libc-2.27.so     memcpy
   12.34%  concurrentcache  concurrentcache  HashTable::find
```

**解决方案**：
1. 优化热点函数
2. 使用内联减少函数调用
3. 使用更高效的数据结构
4. 编译器优化（-O3）

#### 上下文切换

**问题描述**：线程频繁切换，导致性能下降。

**原因分析**：
```
上下文切换开销：
┌─────────────────────────────────────────────────────────────┐
│  保存寄存器         ~1μs                                    │
│  切换页表（TLB）  ~1-100μs                                │
│  刷新CPU缓存       ~10-100μs                               │
│                                                              │
│  如果每秒切换1000次：                                        │
│  → 额外开销可达100ms                                        │
└─────────────────────────────────────────────────────────────┘
```

**解决方案**：
1. 减少线程数量
2. 使用线程绑定CPU核心
3. 使用I/O多路复用替代多线程
4. 增大任务粒度

---

### 7.2.2 内存瓶颈

#### 缓存未命中

**问题描述**：CPU缓存命中率低，导致内存访问延迟高。

**原因分析**：
```
访问模式对缓存命中率的影响：

坏模式（遍历）：
int sum = 0;
for (int i = 0; i < N; i++) {
    sum += array[i];  // 数组元素可能不在缓存中
}

好模式（分块）：
for (int i = 0; i < N; i += BLOCK) {
    for (int j = 0; j < BLOCK; j++) {
        sum += array[i + j];  // 块内元素在缓存中
    }
}
```

**解决方案**：
1. 数据结构设计考虑缓存局部性
2. 使用预取（prefetch）
3. 调整数据布局
4. 使用内存对齐

---

### 7.2.3 I/O瓶颈

#### 小I/O问题

**问题描述**：频繁的小I/O操作导致性能下降。

**原因分析**：
```
每次write都需要系统调用：
write(fd, "a", 1);  // 系统调用开销
write(fd, "b", 1);  // 系统调用开销
write(fd, "c", 1);  // 系统调用开销
```

**解决方案**：
```cpp
// 批量写入
std::string buffer;
buffer += "a";
buffer += "b";
buffer += "c";
write(fd, buffer.data(), buffer.size());  // 一次系统调用
```

#### 磁盘I/O

**问题描述**：持久化操作导致性能下降。

**解决方案**：
1. 使用SSD代替HDD
2. 配置合适的刷盘策略
3. 使用AOF重写合并小写入
4. 使用RDB进行定期快照

---

## 7.3 分布式问题

### 7.3.1 脑裂问题

**问题描述**：网络分区导致集群分裂成多个子集群，每个子集群都认为自己是主节点。

**场景分析**：
```
正常状态：
┌─────────────────────────────────────────┐
│              完整集群（3节点）             │
│  A(M) ←复制→ B(R) ←复制→ C(R)          │
└─────────────────────────────────────────┘

网络分区（假设A和B、C断开）：
┌──────────────────┐   ┌──────────────────┐
│    子集群1       │   │    子集群2       │
│   ┌─────────┐    │   │   ┌─────────┐    │
│   │  A(M)   │    │   │   │  B(R)   │    │
│   │ 独立工作 │    │   │   │ 独立工作 │    │
│   └─────────┘    │   │   └─────────┘    │
│                  │   │   ┌─────────┐    │
│                  │   │   │  C(R)   │    │
│                  │   │   │ 独立工作 │    │
└──────────────────┘   └──────────────────┘

问题：
- A认为B、C故障
- B、C认为A故障
- B升级为主节点
- A还是主节点（但不知道B已升级）
```

**解决方案**：
1. 使用多数派原则（quorum）
2. 配置节点超时时间
3. 网络恢复后合并数据
4. 使用分布式锁

### 7.3.2 数据一致性

**问题描述**：主从复制延迟导致的数据不一致。

**场景分析**：
```
客户端A写入主节点：
主节点: SET key value1
主节点: 复制到从节点

客户端B从从节点读取：
从节点: GET key → 可能还是value（如果复制延迟）

时间线：
T0: 客户端A写入主节点 key=value1
T1: 从节点还未收到复制命令
T2: 客户端B读取，得到旧值
```

**解决方案**：
1. 使用WAIT命令等待复制确认
2. 读取主节点而非从节点（强一致）
3. 读取从节点时带上版本号（最终一致）
4. 监控复制延迟

### 7.3.3 槽迁移冲突

**问题描述**：槽迁移过程中出现数据不一致。

**场景分析**：
```
Node1正在迁移槽12345到Node2：
1. Node1向Node2发送键key1
2. 客户端请求key1，Node1还未发送
3. 客户端访问Node1，得到旧值
4. 键key1被删除（已迁移到Node2）
5. 客户端得到不存在的key
```

**解决方案**：
1. 使用ASK重定向而非MOVED
2. 迁移过程中源节点保留数据
3. 完成后广播槽归属变更
4. 客户端实现ASK重定向处理

---

## 7.4 运维问题

### 7.4.1 性能测试

#### 基准测试

```bash
# 使用redis-benchmark测试
redis-benchmark -h localhost -p 6379 -t set,get -n 100000 -c 100

# 输出示例：
====== SET ======
  100000 requests completed in 1.23 seconds
  100 parallel clients
  50.41% <= 1 milliseconds
  99.99% <= 2 milliseconds
```

#### 压力测试

```bash
# 使用wrk进行HTTP风格测试
wrk -t12 -c400 -d30s http://localhost:6379

# 使用memtier_benchmark
memtier_benchmark -s localhost -p 6379 -t 4 -c 50 -d 1000 --ratio=1:1
```

### 7.4.2 监控

#### 关键指标

| 指标 | 说明 | 告警阈值 |
|-----|------|---------|
| CPU使用率 | CPU占用百分比 | >80% |
| 内存使用率 | 内存占用百分比 | >90% |
| QPS | 每秒请求数 | <1000 |
| 延迟P99 | 99分位延迟 | >10ms |
| 连接数 | 当前连接数 | >8000 |
| 复制延迟 | 主从复制延迟 | >1s |

#### 监控工具

```bash
# 使用INFO命令查看状态
redis-cli INFO
redis-cli INFO stats
redis-cli INFO replication
redis-cli INFO clients

# 使用MONITOR查看实时操作
redis-cli MONITOR
```

### 7.4.3 日志分析

```bash
# 查看错误日志
grep -i error logs/concurrentcache.log | head -100

# 查看慢查询
grep -i "slow" logs/concurrentcache.log

# 按时间范围查看
grep "2024-01-15 10:00" logs/concurrentcache.log | grep -i error

# 使用日志分析工具
goaccess -f logs/access.log
```

---

## 7.5 调试技巧

### 7.5.1 GDB调试

```bash
# 启动调试
gdb ./concurrentcache

# 设置断点
(gdb) break HashTable::set
(gdb) break Connection::handle_read

# 运行
(gdb) run

# 查看变量
(gdb) print key
(gdb) print value

# 单步执行
(gdb) next
(gdb) step

# 查看堆栈
(gdb) bt

# 查看线程
(gdb) info threads
(gdb) thread 2
```

### 7.5.2 核心转储分析

```bash
# 生成核心转储
ulimit -c unlimited
./concurrentcache  # 崩溃
gcore 12345  # 生成核心转储

# 分析核心转储
gdb ./concurrentcache core.12345
(gdb) bt
(gdb) info registers
(gdb) x/100x $sp
```

### 7.5.3 网络抓包

```bash
# 使用tcpdump抓包
tcpdump -i eth0 -w capture.pcap port 6379

# 使用Wireshark分析
wireshark capture.pcap

# 查看特定连接
tcpdump -i eth0 'tcp port 6379' -A
```

---

## 7.6 最佳实践

### 7.6.1 开发规范

```cpp
// 1. 使用RAII管理资源
class Connection {
public:
    Connection(int fd) : fd_(fd) {}
    ~Connection() { close(fd_); }

private:
    int fd_;
};

// 2. 使用智能指针
auto conn = std::make_unique<Connection>(fd);

// 3. 避免裸指针
class Server {
    // 不好：std::vector<Connection*> connections_;
    // 好：
    std::vector<std::unique_ptr<Connection>> connections_;
};

// 4. 使用const
class Cache {
    std::string get(const std::string& key) const {  // const方法
        // ...
    }
};

// 5. 使用override
class SetCommand : public Command {
    void execute(Storage* storage) override {  // 使用override
        // ...
    }
};
```

### 7.6.2 配置建议

```ini
# 生产环境配置示例
# 网络配置
port = 6379
bind = 0.0.0.0
max_clients = 10000
tcp_backlog = 511

# 线程配置
thread_num = 4

# 内存配置
max_memory = 10737418240  # 10GB
maxmemory_policy = allkeys-lru

# 持久化配置
save 900 1   # 900秒内1个key变化则保存
save 300 10  # 300秒内10个key变化则保存
appendonly = yes
appendfsync = everysec

# 日志配置
loglevel = notice
logfile = /var/log/concurrentcache.log
```

### 7.6.3 故障恢复

```bash
# 场景1：进程崩溃，需要恢复数据
# 1. 检查是否有RDB文件
ls -la dump.rdb

# 2. 启动服务，数据会自动加载
./concurrentcache &

# 场景2：需要手动加载RDB
./concurrentcache --load-rdb dump.rdb

# 场景3：数据损坏，需要从副本恢复
# 1. 停止当前主节点
redis-cli SHUTDOWN

# 2. 从从节点获取最新数据
# 3. 将从节点升级为主节点
redis-cli CLUSTER REPLICATE <new-master-id>

# 4. 重新配置其他从节点
```

---

## 文档结束

本开发文档完整介绍了 ConcurrentCache 项目从骨架版本到分布式版本的完整迭代过程。

**学习路径建议**：

1. **第一阶段**：理解骨架版本（V1）
   - 重点：整体架构、组件协作
   - 目标：能回答"一个请求从接收到响应的完整流程"

2. **第二阶段**：理解基础版本（V2）
   - 重点：性能优化思路
   - 目标：能回答"为什么要用内存池？MainSubReactor解决了什么问题？"

3. **第三阶段**：理解增强版本（V3）
   - 重点：数据结构和持久化
   - 目标：能回答"Redis为什么能支持那么多数据类型？"

4. **第四阶段**：理解分布式版本（V4）
   - 重点：分布式系统挑战
   - 目标：能回答"Redis Cluster是怎么做到故障自动转移的？"

---

**祝学习愉快！**
