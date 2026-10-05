// psync_cmd.cpp
#include "psync_cmd.h"
#include "protocol/resp.h"
#include "cluster/cluster_server.h"
#include "cluster/replication_mgr.h"
#include "cluster/cluster_connection.h"
#include "cluster/cluster_gossip.h"
#include "base/log.h"
#include "cache/storage.h"
#include "persistence/rdb.h"
#include <sstream>
#include <chrono>

namespace cc_server {

std::string PsyncCommand::execute(const std::vector<std::string>& args) {
    // PSYNC <runid> <offset>
    // PSYNC ? -1 (full sync request)

    // 本项目的内部复制不走客户端口的 PSYNC：副本是在 CLUSTER REPLICATE 时通过总线
    // 发 "REPLSYNC:<node>"，主节点那边调 ReplicationMgr::send_rdb_to_replica()。
    //
    // 这个处理器原来的行为是：回 "+FULLRESYNC <runid> <offset>"，然后**什么都不发**
    // ——不发 RDB，也不接命令流。真 Redis 拿 REPLICAOF 接上来会得到"握手成功、
    // 零数据、状态 online"，比直接拒绝坏得多（运维以为已经有副本了）。
    // 要对外提供副本服务，得先把 RDB 换成 Redis 的方言（版本字节、类型操作码、
    // EOF + CRC64），那是另一件事；在没做之前必须显式拒绝，见 docs/api.md §13。
    (void)args;
    LOG_WARN(CLUSTER, "PSYNC rejected: 不接受外部 Redis 副本，内部复制走集群总线（REPLSYNC）");
    return RespEncoder::encode_error(
        "ERR this server does not accept external replicas; internal replication uses the cluster bus");
}

std::string SyncCommand::execute(const std::vector<std::string>& args) {
    // 同 PSYNC：不再委托给一个"只握手不回数据"的处理器。
    (void)args;
    LOG_WARN(CLUSTER, "SYNC rejected: 不接受外部 Redis 副本，内部复制走集群总线（REPLSYNC）");
    return RespEncoder::encode_error(
        "ERR this server does not accept external replicas; internal replication uses the cluster bus");
}

std::string ReplconfCommand::execute(const std::vector<std::string>& args) {
    // REPLCONF 是外部副本握手的一部分，本服务器不接受外部副本（见 PsyncCommand）。
    //
    // 这里另外有两条真问题，一并拿掉：
    //   1. "REPLCONF ACK <n>" 把 <n> 直接写进本节点的 master_repl_offset，而 failover
    //      的新鲜度判据读的就是这个值 —— 客户端口上任何进程都能把一个空副本的偏移量
    //      报成很新，让它在选举里被当成合格副本。
    //   2. GETACK 把 "REPLCONF ACK <n>\r\n" 整行当 bulk string 回出去，那是 Redis 的
    //      行协议文本，不是一个 RESP 值。
    (void)args;
    LOG_WARN(CLUSTER, "REPLCONF rejected: 外部副本握手不受支持（原先 ACK 会改写主节点偏移量）");
    return RespEncoder::encode_error(
        "ERR this server does not accept external replicas; internal replication uses the cluster bus");
}

} // namespace cc_server