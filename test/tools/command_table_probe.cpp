// 命令表探针：把候选命令名交给真实的 CommandFactory，报告哪些确实注册了。
//
// 一致性脚本因此不需要信任它对 command_factory.cpp 的正则解析——注册表由
// 运行时自己回答。CI 里没有别的调用方能替代这个位置：产品代码不解析 argv，
// 也没有暴露名单的接口。
#include <iostream>

#include "command/command_factory.h"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        auto cmd = cc_server::CommandFactory::instance().create(argv[i]);
        std::cout << argv[i] << (cmd ? " registered" : " missing") << "\n";
    }
    return 0;
}
