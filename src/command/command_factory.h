#ifndef CONCURRENTCACHE_COMMAND_COMMAND_FACTORY_H
#define CONCURRENTCACHE_COMMAND_COMMAND_FACTORY_H

#include <memory>
#include <unordered_map>
#include "command.h"

namespace cc_server {

    /**
     * @brief CommandFactory - 命令工厂类
     *
     * 工厂模式：根据命令名创建对应的命令对象
     *
     * 为什么需要工厂？
     * - 简单做法：if (cmd == "get") return new GetCommand();
     * - 问题：如果有很多命令，if-else 会很长
     * - 工厂模式：用 map 存储映射，添加新命令只需修改注册部分
     *
     * 使用方法：
     *   CommandFactory::instance().register_command("get", std::make_unique<GetCommand>());
     *   auto cmd = CommandFactory::instance().create("get");
     */
    class CommandFactory {
    public:
        /**
         * @brief 获取单例实例
         *
         * 使用 Magic Static 保证线程安全
         */
        static CommandFactory& instance();

        /**
         * @brief 注册命令
         * @param name 命令名（如 "get", "set"）
         * @param cmd 命令对象（智能指针）
         *
         * 示例：
         *   factory.register_command("get", std::make_unique<GetCommand>());
         */
        void register_command(const std::string& name, std::unique_ptr<Command> cmd);

        /**
         * @brief 创建命令对象
         * @param name 命令名
         * @return 命令对象指针，未找到返回 nullptr
         *
         * 示例：
         *   auto cmd = factory.create("get");
         *   if (cmd) {
         *       std::string resp = cmd->execute({"GET", "key"});
         *   }
         */
        std::unique_ptr<Command> create(const std::string& name);

        // 禁用拷贝，确保单例唯一性
        CommandFactory(const CommandFactory&) = delete;
        CommandFactory& operator=(const CommandFactory&) = delete;

    private:
        /**
         * @brief 私有构造函数
         *
         * 在构造函数中注册所有内置命令
         */
        CommandFactory();

        // 命令注册表：命令名 → 命令对象
        // key: 命令名字符串（如 "get", "set"）
        // value: 命令对象的智能指针
        std::unordered_map<std::string, std::unique_ptr<Command>> commands_;
    };

}

/*
==============================================================================
std::unique_ptr 详解
==============================================================================

1. 什么是 unique_ptr？
   unique_ptr 是 C++11 引入的智能指针，用于管理堆分配的对象。

2. 为什么需要 unique_ptr？
   - 传统方式：T* ptr = new T(); ... delete ptr;
   - 问题：容易忘记 delete，导致内存泄漏
   - unique_ptr：自动管理内存，析构时自动 delete

3. unique_ptr 的特点：
   - 独占所有权：只能有一个 unique_ptr 拥有对象
   - 自动释放：当 unique_ptr 销毁时，自动 delete 所指对象
   - 不能拷贝：unique_ptr<T> 不能复制给另一个 unique_ptr<T>
   - 可以移动：可以通过 std::move 转移所有权

4. make_unique<T>()：
   - 创建 unique_ptr 的推荐方式
   - std::make_unique<T>(args...) 等同于 new T(args...)
   - 示例：std::make_unique<GetCommand>() 等同于 new GetCommand()

5. std::move()：
   - 转移 unique_ptr 的所有权
   - 转移后，原 unique_ptr 变为空
   - 示例：
       std::unique_ptr<int> p1 = std::make_unique<int>(42);
       std::unique_ptr<int> p2 = std::move(p1);  // p1 变为空，p2 拥有对象

6. 在本类中的使用：
   - commands_ 存储 map<string, unique_ptr<Command>>
   - 每个命令只存储一个 unique_ptr
   - create() 时通过 clone() 创建新的命令实例返回

7. 为什么返回 unique_ptr 而不是原始指针？
   - 安全：调用方不需要关心释放内存
   - 异常安全：如果发生异常，unique_ptr 自动释放
   - 语义清晰：表示"唯一所有权"
*/

#endif
