//
// Created by Administrator on 2026/5/5.
//

#ifndef EXPIRATION_CHECKER_H
#define EXPIRATION_CHECKER_H


#include <atomic>
#include <thread>

namespace cc_server {
    class ExpireDict;
    class GlobalStorage;

    /**
     * @brief ExpirationChecker 类 - 定期删除过期键
     *
     * 后台线程每 100ms 检查并删除过期键
     * 避免 CPU 占用过高，单次执行不超过 25ms
     */

    class ExpirationChecker {
    public:
        explicit ExpirationChecker(ExpireDict& expire_dict, GlobalStorage& storage);

        ExpirationChecker(const ExpirationChecker&) = delete;
        ExpirationChecker& operator=(const ExpirationChecker&) = delete;


        // 启动定期检查
        void start();

        // 停止定期检查
        void stop();

        // 析构 确保线程结束
        ~ExpirationChecker();

    private:
        // 检查线程主循环
        void run();

        ExpireDict& expire_dict_;  // 引用过期字典
        GlobalStorage& storage_;   // 引用存储（用于删除过期数据）
        std::atomic<bool> running_; // 运行标志
        std::thread thread_; // 检查线程

        static constexpr int kCheckIntervalMs = 100; // 检查间隔
        static constexpr int kMaxCheckDurationMs = 25; // 单次检查最大持续
    };
}
#endif //EXPIRATION_CHECKER_H
