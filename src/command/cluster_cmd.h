#ifndef CONCURRENTCACHE_CLUSTER_CMD_H
#define CONCURRENTCACHE_CLUSTER_CMD_H

#include "command.h"

namespace cc_server {

class ClusterCommand : public Command {
public:
    ClusterCommand() = default;
    ~ClusterCommand() override = default;

    // 执行 CLUSTER <subcommand> [args...]
    std::string execute(const std::vector<std::string>& args) override;

    // 克隆自己
    std::unique_ptr<Command> clone() const override {
        return std::make_unique<ClusterCommand>();
    }

private:
    // 处理子命令
    std::string handleMeet(const std::vector<std::string>& args);
    std::string handleNodes(const std::vector<std::string>& args);
    std::string handleInfo(const std::vector<std::string>& args);
    std::string handleAddSlots(const std::vector<std::string>& args);
    std::string handleSlots(const std::vector<std::string>& args);
    std::string handleDelSlots(const std::vector<std::string>& args);
    std::string handleSetSlot(const std::vector<std::string>& args);
    std::string handleReplicate(const std::vector<std::string>& args);
    std::string handleFail(const std::vector<std::string>& args);
    std::string handleMigrate(const std::vector<std::string>& args);

    // 验证 IP 地址格式
    static bool isValidIp(const std::string& ip);
};

} // namespace cc_server

#endif
