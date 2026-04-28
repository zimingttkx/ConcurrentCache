// 线程池职责：
// 1. 预先创建固定数量的工作线程
// 2. 线程不断从任务队列取任务执行
// 3. 无任务时线程阻塞，不消耗 CPU
// 4. 支持优雅退出
//
// 线程安全机制：
// 1. 任务队列用 mutex 保护
// 2. 条件变量通知线程有新任务
// 3. 使用 atomic 标记退出状态
//
// 死锁避免策略：
// 1. RAII 自动释放锁（LockGuard）
// 2. 锁的粒度尽量小（只保护必要的操作）
// 3. 避免在持锁时调用用户回调（防止用户代码死锁）
//

#ifndef CONCURRENTCACHE_THREAD_POOL_H
#define CONCURRENTCACHE_THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <future>

namespace cc_server {

/**
 * @brief 线程池类
 *
 * 使用方法：
 * @code
 *   ThreadPool pool(4);  // 创建 4 个工作线程
 *
 *   // 提交任务（返回 future，可以获取返回值）
 *   auto future = pool.submit([]() {
 *       return 1 + 2;
 *   });
 *   int result = future.get();  // 获取结果
 *
 *   // 或者提交 void 任务
 *   pool.submit([]() {
 *       do_something();
 *   });
 *
 *   // 程序结束前销毁线程池
 *   pool.stop();  // 可选，会自动调用
 * @endcode
 *
 * 线程安全分析：
 * - 任务队列：多个线程可能同时 push/pop，用 mutex 保护
 * - 退出标志 stop_：多个线程可能同时读写，用 atomic
 * - 条件变量：配合 mutex 用于线程间通信
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数
     * @param num_threads 工作线程数量
     *
     * 注意：
     * - 如果 num_threads == 0，会自动设置为 1
     * - 创建后立即启动所有线程
     */
    explicit ThreadPool(size_t num_threads = 1);

    /**
     * @brief 析构函数
     *
     * 自动调用 stop() 确保线程安全退出
     */
    ~ThreadPool();

    // 禁止拷贝（线程池不能复制）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务的核心接口
    /**
     * @brief 提交一个任务到线程池
     * @tparam F  callable 类型（函数、lambda、bind 表达式等）
     * @tparam Args 可变参数类型
     * @param f 要执行的可调用对象
     * @param args 传递给 f 的参数
     * @return std::future<T> 用于获取任务返回值
     *
     * 使用示例：
     * @code
     *   // 无参数任务
     *   pool.submit([]() {
     *       printf("hello\\n");
     *   });
     *
     *   // 有参数任务
     *   pool.submit([](int x, int y) {
     *       return x + y;
     *   }, 1, 2);
     *
     *   // 获取返回值
     *   auto future = pool.submit([]() -> int {
     *       return 42;
     *   });
     *   int result = future.get();  // 阻塞直到任务完成，返回 42
     * @endcode
     *
     * 线程安全：
     * - push 任务到队列时加锁
     * - 添加成功后通知一个等待中的线程
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    // 控制接口
    /**
     * @brief 停止线程池
     *
     * 流程：
     * 1. 设置 stop_ = true（所有线程都会看到这个标志）
     * 2. 通知所有等待中的线程（让它们退出）
     * 3. 等待所有线程结束
     *
     * 注意：
     * - 调用后不能再提交新任务
     * - 已提交的任务会继续执行完成
     * - 可以多次调用（后续调用无效）
     */
    void stop();

    /**
     * @brief 获取当前工作线程数量
     */
    size_t num_threads() const { return threads_.size(); }

    /**
     * @brief 获取队列中等待的任务数量
     *
     * 注意：
     * - 这是近似值，调用时可能有其他线程修改队列
     * - 主要用于调试和监控
     */
    size_t queue_size() const;

private:
    // 内部数据结构

    /**
     * @brief 任务基类
     *
     * 使用继承是为了让 std::function<void()> 可以存储任何可调用对象
     */
    struct Task {
        virtual ~Task() = default;
        virtual void run() = 0;
    };

    /**
     * @brief 模板任务类
     * @tparam T 返回类型
     *
     * 存储用户的 callable 和 promise，用于返回结果
     */
    template<typename T>
    struct TaskImpl : public Task {
        std::function<T()> func;       // 用户要执行的函数
        std::promise<T> promise;       // 用于返回结果

        explicit TaskImpl(std::function<T()> f) : func(std::move(f)) {}

        void run() override {
            // 执行函数，结果存入 promise
            // 如果函数抛出异常，promise 也会存储异常
            try {
                if constexpr (std::is_void_v<T>) {
                    func();
                    promise.set_value();
                } else {
                    promise.set_value(func());
                }
            } catch (...) {
                // 捕获任何异常，避免异常传播导致 std::terminate
                promise.set_exception(std::current_exception());
            }
        }
    };

    // 工作线程函数
    /**
     * @brief 工作线程主循环
     * @param thread_id 线程标识（用于日志）
     *
     * 工作流程：
     * while (true) {
     *     1. 获取任务（加锁）
     *     2. 如果队列空，阻塞等待
     *     3. 如果收到退出信号，退出循环
     *     4. 否则，取出任务，释放锁
     *     5. 执行任务
     * }
     *
     * 死锁避免：
     * - 锁的粒度尽量小：只在操作队列时加锁
     * - 执行任务时不持锁：允许其他线程同时执行任务
     */
    void worker_loop(size_t thread_id);

    /**
     * @brief 获取一个任务（会阻塞直到获取到或退出）
     * @return 任务指针，如果退出则返回 nullptr
     *
     * 线程安全：
     * - 操作队列时持有 mutex_
     * - 队列空时等待 condition_variable_
     * - 退出时立即返回 nullptr
     */
    std::unique_ptr<Task> take_task();

    // 工作线程列表
    std::vector<std::thread> threads_;

    // 任务队列
    std::queue<std::unique_ptr<Task>> tasks_;

    // 保护任务队列的互斥锁
    // - 任务队列操作简单（push/pop），不需要读写分离
    // - 这里用 mutex 配合 condition_variable 实现阻塞等待
    mutable std::mutex queue_mutex_;

    // 条件变量：用于线程间通信
    // 用途：
    // - 队列空时，工作线程等待（不消耗 CPU）
    // - 有新任务时，主线程通知唤醒
    std::condition_variable cv_;

    // 停止标志
    // - atomic 保证读写原子性
    // - 无需加锁即可安全读写
    std::atomic<bool> stop_{false};

    // 活跃线程计数
    // 用于判断所有线程是否都已退出
    std::atomic<size_t> active_threads_{0};

    // 已提交任务计数（用于调试/监控）
    std::atomic<uint64_t> submitted_tasks_{0};
};

} // namespace cc_server

#endif // CONCURRENTCACHE_THREAD_POOL_H
