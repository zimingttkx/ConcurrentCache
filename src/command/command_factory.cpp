#include "command_factory.h"
#include "string_cmd.h"

namespace cc_server {

    // =========================================================================
    // 私有构造函数：自动注册所有内置命令
    // =========================================================================
    //
    // 为什么在这里注册命令？
    // - 每次创建 CommandFactory 实例时，自动注册所有支持的命令
    // - 由于是单例模式，只会有一个实例，所以只注册一次
    //
    CommandFactory::CommandFactory() {
        // 注册内置命令（使用小写，兼容 Redis 协议）
        // 注意：Redis 命令不区分大小写，但业界通常用小写存储
        //
        // std::make_unique<GetCommand>() 创建了一个 unique_ptr<GetCommand>
        // 等同于：std::unique_ptr<Command>(new GetCommand())
        // 但更安全，推荐使用
        register_command("get", std::make_unique<GetCommand>());
        register_command("set", std::make_unique<SetCommand>());
        register_command("del", std::make_unique<DelCommand>());
        register_command("exists", std::make_unique<ExistsCommand>());
    }

    // =========================================================================
    // instance() - 获取单例实例
    // =========================================================================
    //
    // 实现：Magic Static（C++11 线程安全局部静态变量）
    //
    // 线程安全保证：
    // - C++11 保证局部静态变量初始化是线程安全的
    // - 多个线程同时调用只会初始化一次
    //
    CommandFactory& CommandFactory::instance() {
        static CommandFactory instance;
        return instance;
    }

    // =========================================================================
    // register_command() - 注册命令
    // =========================================================================
    //
    // 参数：
    //   name - 命令名（如 "get", "set"）
    //   cmd  - 命令对象的 unique_ptr（智能指针）
    //
    // 实现：
    //   把命令名和命令对象的映射存入 commands_ map
    //
    // std::move(cmd) 的作用：
    //   - unique_ptr 不能复制，只能移动
    //   - std::move(cmd) 把 cmd 的所有权转移到 map 中
    //   - 转移后，cmd 变为空，不能再使用
    //
    void CommandFactory::register_command(const std::string& name,
                                          std::unique_ptr<Command> cmd) {
        // commands_[name] = cmd;  // 错误：unique_ptr 不能复制
        commands_[name] = std::move(cmd);  // 正确：移动语义
    }

    // =========================================================================
    // create() - 创建命令对象
    // =========================================================================
    //
    // 参数：
    //   name - 命令名
    //
    // 返回：
    //   成功：返回新的命令对象（unique_ptr）
    //   失败：返回 nullptr（命令不存在）
    //
    // 实现流程：
    //   1. 在 commands_ map 中查找命令名
    //   2. 如果找到，使用 clone() 创建新实例返回
    //   3. 如果没找到，返回 nullptr
    //
    std::unique_ptr<Command> CommandFactory::create(const std::string& name) {
        // 在 map 中查找命令
        auto it = commands_.find(name);

        if (it != commands_.end()) {
            // 找到命令，使用 clone() 创建新实例
            // 为什么需要 clone？
            // - commands_ 存储的是唯一实例
            // - 如果直接返回 it->second，多个连接会共用同一个命令对象
            // - 命令对象可能有内部状态，共用会导致问题
            // - clone() 创建新实例，保证每个请求独立
            return it->second->clone();
        }

        // 命令不存在
        return nullptr;
    }

    /*
    ==========================================================================
    std::unique_ptr 补充说明
    ==========================================================================

    1. 与原始指针的对比：

       // 原始指针方式（不推荐）
       Command* create(const std::string& name) {
           if (name == "get") return new GetCommand();
           return nullptr;
       }
       // 调用方必须记得 delete，否则内存泄漏
       auto cmd = create("get");
       cmd->execute(args);
       delete cmd;  // 容易忘记！

       // unique_ptr 方式（推荐）
       std::unique_ptr<Command> create(const std::string& name) {
           if (name == "get") return std::make_unique<GetCommand>();
           return nullptr;
       }
       // 调用方不需要 delete，出了作用域自动释放
       auto cmd = create("get");
       cmd->execute(args);
       // cmd 离开作用域，自动 delete

    2. unique_ptr 的成员访问：

       - it->second 获取 map 中存储的 unique_ptr 引用
       - it->second->clone() 调用 unique_ptr 所指对象的 clone() 方法
       - 返回值是新的 unique_ptr，所有权转移给调用方

    3. 为什么 clone() 返回 unique_ptr：

       - 保证每个命令实例独立
       - 调用方获得所有权，不用关心释放
       - 符合 RAII 原则
    */
}
