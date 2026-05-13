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
    // 或 PSYNC ? -1 (full sync)

    // 检查是否启用集群模式
    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR REPLICAOF is not supported in standalone mode");
    }

    // 检查本节点是否是从节点
    if (!ClusterServer::instance().isReplica()) {
        // 主节点收到 PSYNC：作为副本的复制请求处理
        // TODO: 支持主节点处理 PSYNC 请求
        LOG_WARN(CLUSTER, "PSYNC received but node is not a replica");
        return RespEncoder::encode_error("ERR PSYNC only valid for replica nodes");
    }

    // 获取主节点信息
    auto master = ClusterServer::instance().getMyMaster();
    if (!master) {
        return RespEncoder::encode_error("ERR no master defined");
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

    // 获取复制管理器
    auto& repl_mgr = ReplicationMgr::instance();

    // 检查是否需要全量同步
    if (runid == "?" || offset == -1) {
        // 全量同步请求
        LOG_INFO(CLUSTER, "Full sync requested");

        // 设置同步状态为等待
        repl_mgr.set_sync_state(SyncState::kWaiting);

        // 发送 RDB 数据
        // 格式：+FULLRESYNC <runid> <offset>\r\n
        // 然后发送 RDB 文件内容
        std::string runid_str = repl_mgr.get_master_runid();
        int64_t repl_offset = repl_mgr.get_master_repl_offset();

        std::string response = "+FULLRESYNC " + runid_str + " " + std::to_string(repl_offset) + "\r\n";
        LOG_INFO(CLUSTER, "Sending FULLRESYNC: runid=%s, offset=%ld", runid_str.c_str(), repl_offset);

        // TODO: 实际发送 RDB 文件内容
        // 暂时返回 CONTINUE，让复制继续进行
        return RespEncoder::encode_simple_string("CONTINUE");
    } else {
        // 增量同步请求
        LOG_INFO(CLUSTER, "Partial sync requested: runid=%s, offset=%ld", runid.c_str(), offset);

        // 检查 runid 是否匹配
        if (runid != repl_mgr.get_master_runid()) {
            // runid 不匹配，需要全量同步
            LOG_WARN(CLUSTER, "Runid mismatch: expected=%s, got=%s",
                     repl_mgr.get_master_runid().c_str(), runid.c_str());
            return RespEncoder::encode_simple_string("CONTINUE");
        }

        // 返回增量同步的确认
        return RespEncoder::encode_simple_string("CONTINUE");
    }
}

std::string SyncCommand::execute(const std::vector<std::string>& args) {
    // SYNC 命令（兼容旧版 Redis，等同于 PSYNC ? -1）
    // 简化实现：直接转发给 PSYNC 处理
    (void)args;  // 未使用参数

    LOG_INFO(CLUSTER, "SYNC command received (legacy)");

    // 检查集群模式
    if (!ClusterServer::instance().isEnabled()) {
        return RespEncoder::encode_error("ERR SYNC not supported in standalone mode");
    }

    // 如果是从节点，返回错误
    if (ClusterServer::instance().isReplica()) {
        return RespEncoder::encode_error("ERR SYNC invalid for replica");
    }

    // SYNC 用于从节点主动触发同步，这里简单返回
    return RespEncoder::encode_simple_string("OK");
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