#ifndef CONCURRENTCACHE_COMMAND_COMMAND_H
#define CONCURRENTCACHE_COMMAND_COMMAND_H

#include <string>
#include <vector>

namespace cc_server {
    class Command {
    public:

                /**
         * @brief Command 基类 - 所有命令的抽象接口
         *
         * 命令模式：把"请求"封装成"对象"
         * - 请求 = 一个 Redis 命令（如 GET, SET, DEL）
         * - 对象 = Command 子类的实例
         *
         * 优点：
         * - 命令的参数、执行逻辑封装在 Command 对象里
         * - 可以存储命令、撤销命令、队列化命令
         * - 易于扩展新命令（添加新子类即可）
         */
        virtual ~Command() = default;

            /**
         * @brief 执行命令
         * @param args 命令参数，格式为 [命令名, 参数1, 参数2, ...]
         *             例如：["GET", "key"] 或 ["SET", "key", "value"]
         * @return RESP 格式的响应字符串
         *
         * 示例：
         *   SetCommand cmd;
         *   std::string resp = cmd.execute({"SET", "name", "zhangsan"});
         *   // resp = "+OK\r\n"
         */
        virtual std::string execute(const std::vector<std::string>& args) = 0;

        /** @brief 克隆命令对象，用于工厂模式创建实例 */
        virtual std::unique_ptr<Command> clone() const = 0;
    };
}
#endif
