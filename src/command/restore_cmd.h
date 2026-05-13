//
// Created by Administrator on 2026/5/13.
//

#ifndef CONCURRENTCACHE_RESTORE_CMD_H
#define CONCURRENTCACHE_RESTORE_CMD_H

#include "command.h"
#include "cache/storage.h"
#include "datatype/object.h"

namespace cc_server {

class RestoreCommand : public Command {
public:
    RestoreCommand() = default;
    ~RestoreCommand() override = default;

    std::string execute(const std::vector<std::string>& args) override;

    std::unique_ptr<Command> clone() const override {
        return std::make_unique<RestoreCommand>();
    }
};

} // namespace cc_server

#endif //CONCURRENTCACHE_RESTORE_CMD_H
