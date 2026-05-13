// psync_cmd.h
#ifndef CONCURRENTCACHE_PSYNC_CMD_H
#define CONCURRENTCACHE_PSYNC_CMD_H

#include "command/command.h"
#include <memory>
#include <string>
#include <vector>

namespace cc_server {

// PSYNC 命令处理类
// 用法：PSYNC <runid> <offset>
//       PSYNC ? -1  (full sync)
//       PSYNC <runid> <offset>  (partial sync)
class PsyncCommand : public Command {
public:
    PsyncCommand() = default;
    ~PsyncCommand() override = default;

    // 执行 PSYNC 命令
    // 参数：PSYNC runid offset
    std::string execute(const std::vector<std::string>& args) override;

    // 克隆接口
    std::unique_ptr<Command> clone() const override {
        return std::make_unique<PsyncCommand>();
    }
};

// SYNC 命令（兼容旧版 Redis）
class SyncCommand : public Command {
public:
    SyncCommand() = default;
    ~SyncCommand() override = default;

    std::string execute(const std::vector<std::string>& args) override;

    std::unique_ptr<Command> clone() const override {
        return std::make_unique<SyncCommand>();
    }
};

// REPLCONF 命令（复制配置）
// 用法：REPLCONF Listening-Port <port>
//       REPLCONF ACK <offset>
//       REPLCONF GETACK <ack_offset>
class ReplconfCommand : public Command {
public:
    ReplconfCommand() = default;
    ~ReplconfCommand() override = default;

    std::string execute(const std::vector<std::string>& args) override;

    std::unique_ptr<Command> clone() const override {
        return std::make_unique<ReplconfCommand>();
    }
};

} // namespace cc_server

#endif // CONCURRENTCACHE_PSYNC_CMD_H