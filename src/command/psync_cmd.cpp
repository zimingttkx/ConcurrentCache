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

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR REPLICAOF is not supported in standalone mode");
    }

    // 解析参数
    std::string runid;
    int64_t offset = -1;

    if (args.size() >= 2) {
        runid = args[1];
    }
    if (args.size() >= 3) {
        try {
            offset = std::stoll(args[2]);
        } catch (...) {
            // ignore parse error
        }
    }

    LOG_INFO(CLUSTER, "PSYNC received: runid=%s, offset=%ld", runid.c_str(), offset);

    auto& repl_mgr = ReplicationMgr::instance();

    // 分支1: 本节点是主节点 — 收到副本发来的 PSYNC 同步请求
    if (!ClusterServer::instance().isReplica()) {
        std::string my_runid = repl_mgr.get_master_runid();
        int64_t my_offset = repl_mgr.get_master_repl_offset();

        // 全量同步: runid 为 "?" 或 offset 为 -1，或 runid 不匹配
        if (runid == "?" || offset == -1 || runid != my_runid) {
            std::string response = "+FULLRESYNC " + my_runid + " " + std::to_string(my_offset) + "\r\n";
            LOG_INFO(CLUSTER, "Sending FULLRESYNC to replica: runid=%s, offset=%ld",
                     my_runid.c_str(), my_offset);
            // RESP 简单字符串格式: +FULLRESYNC <runid> <offset>\r\n
            // 使用 bulk string 编码以便客户端正确解析
            return RespEncoder::encode_simple_string("FULLRESYNC " + my_runid + " " + std::to_string(my_offset));
        }

        // 增量同步: runid 匹配
        LOG_INFO(CLUSTER, "Partial sync: runid matches, offset=%ld, my_offset=%ld",
                 offset, my_offset);
        return RespEncoder::encode_simple_string("CONTINUE");
    }

    // 分支2: 本节点是从节点 — 收到来自主节点的 PSYNC 响应 (不常见，主节点不会主动发 PSYNC)
    LOG_WARN(CLUSTER, "PSYNC received on replica node, ignoring");
    return RespEncoder::encode_error("ERR PSYNC not expected on replica node");
}

std::string SyncCommand::execute(const std::vector<std::string>& args) {
    // SYNC 命令（兼容旧版 Redis，等同于 PSYNC ? -1）
    // 直接委托给 PSYNC 全量同步逻辑
    (void)args;

    LOG_INFO(CLUSTER, "SYNC command received (legacy, delegating to PSYNC)");

    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR SYNC not supported in standalone mode");
    }

    // 构建 PSYNC ? -1 参数列表，委托给 PsyncCommand
    std::vector<std::string> psync_args = {"PSYNC", "?", "-1"};
    PsyncCommand psync_cmd;
    return psync_cmd.execute(psync_args);
}

std::string ReplconfCommand::execute(const std::vector<std::string>& args) {
    // REPLCONF 命令处理
    // 格式：
    //   REPLCONF listening-port <port>
    //   REPLCONF ACK <offset>
    //   REPLCONF GETACK <ack_offset>

    if (args.size() < 2) {
        return RespEncoder::encode_error("ERR wrong number of arguments for 'replconf' command");
    }

    const std::string& subcmd = args[1];

    if (subcmd == "listening-port" && args.size() >= 3) {
        // 副本报告监听的端口
        std::string port_str = args[2];
        LOG_DEBUG(CLUSTER, "REPLCONF listening-port: %s", port_str.c_str());
        return RespEncoder::encode_simple_string("OK");

    } else if (subcmd == "ACK" && args.size() >= 3) {
        // 副本确认已处理的复制偏移量
        std::string offset_str = args[2];
        try {
            int64_t offset = std::stoll(offset_str);
            ReplicationMgr::instance().set_master_repl_offset(offset);
            LOG_DEBUG(CLUSTER, "REPLCONF ACK: offset=%ld", offset);
        } catch (...) {
            // ignore
        }
        return RespEncoder::encode_simple_string("OK");

    } else if (subcmd == "GETACK" && args.size() >= 3) {
        // 主节点请求副本确认偏移量
        std::string ack_offset_str = args[2];
        int64_t ack_offset = 0;
        try {
            ack_offset = std::stoll(ack_offset_str);
        } catch (...) {
            // ignore
        }

        int64_t master_offset = ReplicationMgr::instance().get_master_repl_offset();
        std::string response = "REPLCONF ACK " + std::to_string(master_offset) + "\r\n";
        LOG_DEBUG(CLUSTER, "REPLCONF GETACK: requested=%ld, current=%ld", ack_offset, master_offset);

        return RespEncoder::encode_bulk_string(response);

    } else {
        return RespEncoder::encode_error("ERR syntax error in REPLCONF command");
    }
}

} // namespace cc_server