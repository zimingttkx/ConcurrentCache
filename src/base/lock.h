//
// Created by dingziming on 2026/4/26.
//

#ifndef CONCURRENTCACHE_LOCK_H
#define CONCURRENTCACHE_LOCK_H

/**
 * @file lock.h
 * @brief 版本2锁机制 - 头文件
 *
为什么要使用锁？
 * 在多线程程序中，多个线程可能同时访问共享资源（比如同一个变量、一个链表等）。
 * 如果不加控制，就会出现数据竞争（data race），导致数据不一致。
 *
 * 举例说明：
 * ┌─────────────────────────────────────────────────────────────┐
 * // 线程A和线程B同时执行下面的代码                          │
 * // 初始时 value = 0                                         │
 *                                                             │
 * // 线程A读取 value (得到0)                                  │
 * // 线程B读取 value (得到0)                                  │
 * // 线程A计算 value + 1 (得到1)                             │
 * // 线程B计算 value + 1 (得到1)                             │
 * // 线程A写入 value = 1                                      │
 * // 线程B写入 value = 1                                      │
 *                                                             │
 * // 结果：value = 1（但我们期望是2！）                        │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 解决方案：用锁把"读取-修改-写入"这个操作变成原子的，
 * 同一时刻只有一个线程能执行这段代码。
 *
 * 1. 新增原子操作封装
 *    - AtomicInteger：原子整数，比 mutex 更轻量
 *    - AtomicPointer：原子指针，用于无锁数据结构
 *
 * 2. 新增多种锁
 *    - Mutex：基本互斥锁（增强版，支持超时获取）
 *    - SpinLock：自旋锁，适合极短临界区
 *    - RecursiveMutex：递归锁，同一线程可多次获取
 *    - RWLock：读写锁，读可以并发，写必须独占
 *    - RWLock2：改进读写锁，支持写优先（避免写饥饿）
 *
 * 3. 新增分片锁
 *    - ShardedLock：分片锁数组，减少锁竞争
 *    - ShardedRWLock：分片读写锁数组
 *
 * 4. 新增同步原语
 *    - Semaphore：信号量，控制同时访问的线程数量
 *    - CountDownLatch：倒计时门栓，等待N个操作完成
 *    - CyclicBarrier：循环屏障，N个线程互相等待
 *
 * 5. 新增RAII锁守卫
 *    - LockGuard：自动获取/释放锁
 *    - TryLockGuard：try_lock的RAII封装
 *    - ReadLockGuard：读锁RAII封装
 *    - WriteLockGuard：写锁RAII封装
 *
 * =============================================================================
 * 常见问题
 * =============================================================================
 *
 * Q: 什么时候用Mutex？什么时候用SpinLock？
 * A: - 临界区代码执行时间 > 1000条CPU指令 → 用Mutex（让线程睡眠，不消耗CPU）
 *    - 临界区代码执行时间 < 100条CPU指令 → 用SpinLock（自旋等待，不挂起线程）
 *
 * Q: 什么是死锁？如何避免？
 * A: - 死锁：两个线程互相等待对方释放锁，导致程序卡住
 *    - 避免方法：1) 用RAII锁守卫（自动释放锁）2) 总是按相同顺序获取锁
 *
 * Q: 什么是读写锁？为什么需要？
 * A: - 读操作不修改数据，多个线程同时读是安全的
 *    - 写操作会修改数据，必须独占
 *    - 读写锁允许 N个读线程 同时访问，但写操作时完全独占
 *
 * Q: 什么是条件变量？
 * A: - 用于线程间通信，一个线程等待某个条件满足才继续执行
 *    - 比如：生产者-消费者模型，消费者等待队列不为空
 */

#include <mutex>              // std::mutex：基本互斥锁
#include <atomic>             // std::atomic：原子类型
#include <shared_mutex>        // std::shared_mutex：C++17读写锁
#include <thread>             // std::thread：线程，std::this_thread
#include <chrono>             // std::chrono：时间相关，如超时
#include <condition_variable> // std::condition_variable：条件变量
#include <vector>             // std::vector：动态数组
#include <string>             // std::string：字符串
#include <limits>             // std::numeric_limits：数值极限

namespace cc_server {

// 平台特定的CPU指令优化

/**
 * @brief CPU_PAUSE 宏 - 提示CPU当前在自旋等待
 *
 * 为什么需要这个宏？
 * - 在自旋锁中，我们需要让CPU"稍等一下"而不是疯狂空转
 * - 不同CPU架构有不同的指令来实现这个功能
 *
 * 详细解释：
 * - x86_64 (大多数桌面/服务器CPU)：使用 pause 指令
 *   → 这个指令会提示CPU当前在忙等，可以让CPU降低功耗
 *   → 同时不会像 nop 一样浪费执行单元
 *
 * - ARM64 (手机、苹果M系列)：使用 yield 指令
 *   → 提示CPU当前在等待，可以让出执行单元
 *
 * - 其他架构：什么都不做（do {} while(0) 是空操作）
 *
 * 为什么不用 std::this_thread::yield()？
 * - yield() 会让出整个时间片，可能导致不必要的上下文切换
 * - pause/yield 只是提示CPU，不让出时间片，效率更高
 */
#ifdef __x86_64__
    // x86_64 架构使用 Intel 的 pause 指令
    // __builtin_ia32_pause() 是 GCC/Clang 提供的内联函数
    #define CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    // ARM64 架构使用 yield 指令
    // __builtin_arm_yield() 是 GCC/Clang 提供的内联函数
    #define CPU_PAUSE() __builtin_arm_yield()
#else
    // 未知架构，使用空操作
    // do {} while(0) 是一个空循环，编译时会优化成什么都不做
    #define CPU_PAUSE() do {} while(0)
#endif


// 原子操作封装


/**
 * @brief AtomicInteger 类 - 原子整数封装
 *
 * 什么是原子操作？
 * - 原子（atomic）意味着"不可分割"
 * - 原子操作在执行过程中不会被其他线程干扰
 * - 从外部看，原子操作要么完全执行，要么完全不执行，不会出现"半执行"状态
 *
 * 为什么需要原子整数？
 * - 计数器是最常见的共享资源
 * - 比如：访问次数统计、连接数统计、序列号生成
 * - 用 mutex 保护计数器也可以，但更慢（需要加锁/解锁）
 * - 原子操作在硬件层面保证原子性，比锁更轻量
 *
 * 内存顺序（memory_order）解释：
 * - std::memory_order_acquire：获取操作，之后的读写不能重排到此操作之前
 * - std::memory_order_release：释放操作，之前的读写不能重排到此操作之后
 * - std::memory_order_acq_rel：获取+释放组合
 *
 * 示例：
 * @code
 *   AtomicInteger counter(0);  // 初始值0
 *
 *   // 线程A执行
 *   counter++;  // 原子加1
 *
 *   // 线程B执行
 *   counter.fetch_add(10);  // 原子加10
 *
 *   // 线程C执行
 *   int val = counter.load();  // 读取当前值
 * @endcode
 */
class AtomicInteger {
public:
    /**
     * @brief 构造函数
     * @param initial_value 初始值，默认为0
     */
    explicit AtomicInteger(int initial_value = 0) noexcept;

    // 禁用拷贝构造函数和拷贝赋值运算符
    // 原子对象不能拷贝，因为拷贝会失去原子性
    AtomicInteger(const AtomicInteger&) = delete;
    AtomicInteger& operator=(const AtomicInteger&) = delete;

    /**
     * @brief 读取当前值
     * @return 当前存储的整数值
     *
     * 使用 memory_order_acquire：
     * - 保证在读取之前的所有操作都可见
     * - 防止指令重排
     */
    [[nodiscard]]int load() const noexcept;

    /**
     * @brief 写入新值
     * @param val 要写入的值
     *
     * 使用 memory_order_release：
     * - 保证这个写入之前的所有操作都先完成
     * - 后续的读取能看到这个写入之前的操作
     */
    void store(int val) noexcept;

    /**
     * @brief 原子交换
     * @param new_val 新值
     * @return 原来的旧值
     *
     * 这是一个原子操作：
     * - 读取原值
     * - 写入新值
     * - 返回原值
     * 三步一起完成，不会有其他线程插入
     */
    int exchange(int new_val) noexcept;

    /**
     * @brief 比较并交换（CAS）- 弱版本
     * @param expected 期望的旧值（传入传出参数）
     * @param desired 要写入的新值
     * @return true 交换成功，false 交换失败
     *
     * CAS 是实现无锁数据结构的核心！
     *
     * 工作原理：
     * - 如果当前值 == expected，说明没有被其他线程修改
     *   → 把当前值改成 desired，返回 true
     * - 如果当前值 != expected，说明被其他线程修改了
     *   → 把 expected 更新为当前值，返回 false
     *
     * 为什么需要弱版本？
     * - 弱版本在某些CPU上更快，但可能"假失败"（明明可以成功却返回失败）
     * - 如果循环中使用，弱版本更好（失败后会重试）
     * - 如果只执行一次，强版本更合适
     *
     * 示例：无锁计数器++
     * @code
     *   int expected = counter.load();
     *   while (!counter.compare_exchange_weak(expected, expected + 1)) {
     *       // 如果失败，expected 已经被更新为当前值，重试
     *   }
     * @endcode
     */
    bool compare_exchange(int& expected, int desired) noexcept;

    /**
     * @brief 比较并交换（CAS）- 强版本
     * @param expected 期望的旧值
     * @param desired 要写入的新值
     * @return true 交换成功，false 交换失败
     *
     * 强版本保证不会假失败，但某些CPU上更慢
     */
    bool compare_exchange_strong(int& expected, int desired) noexcept;

    /**
     * @brief 原子加法
     * @param arg 要加的值（可以是负数实现减法）
     * @return 返回增加前的原值
     */
    int fetch_add(int arg) noexcept;

    /**
     * @brief 原子减法
     * @param arg 要减的值
     * @return 返回减少前的原值
     */
    int fetch_sub(int arg) noexcept;

    /**
     * @brief 原子按位与
     * @param arg 要按位与的值
     * @return 操作前的原值
     */
    int fetch_and(int arg) noexcept;

    /**
     * @brief 原子按位或
     * @param arg 要按位或的值
     * @return 操作前的原值
     */
    int fetch_or(int arg) noexcept;

    /**
     * @brief 原子按位异或
     * @param arg 要按位异或的值
     * @return 操作前的原值
     */
    int fetch_xor(int arg) noexcept;

    // 便捷操作符

    /**
     * @brief 前置++
     * @return 增加后的值
     */
    int operator++() noexcept;

    /**
     * @brief 后置++
     * @return 增加前的原值
     */
    int operator++(int) noexcept;

    /**
     * @brief 前置--
     * @return 减少后的值
     */
    int operator--() noexcept;

    /**
     * @brief 后置--
     * @return 减少前的原值
     */
    int operator--(int) noexcept;

    /**
     * @brief 加法赋值
     * @param arg 要加的值
     * @return 增加后的值
     */
    int operator+=(int arg) noexcept;

    /**
     * @brief 减法赋值
     * @param arg 要减的值
     * @return 减少后的值
     */
    int operator-=(int arg) noexcept;

    /**
     * @brief 自动转换为int
     * @return 当前值
     */
    explicit operator int() const noexcept;

private:
    std::atomic<int> value_;  // 底层原子变量
};

/**
 * @brief AtomicPointer 类 - 原子指针封装
 *
 * 用于无锁数据结构中的指针操作
 *
 * 什么是指针的原子操作？
 * - 在多线程环境中，安全地加载或存储一个指针
 * - 普通指针的读写不是线程安全的
 * - AtomicPointer 保证指针读写的原子性
 *
 * T* 使用默认模板参数 nullptr_t，
 * 但实际使用中 T 是任意类型
 *
 * 示例：
 * @code
 *   AtomicPointer<Node> head;  // 指向链表头
 *
 *   // 线程A：原子地替换头节点
 *   Node* new_node = new Node();
 *   new_node->next = head.load();
 *   head.store(new_node);
 *
 *   // 线程B：原子地读取头节点
 *   Node* current = head.load();
 *   if (current) {
 *       process(current->data);
 *   }
 * @endcode
 *
 * @tparam T 指针指向的类型
 */
template<typename T>
class AtomicPointer {
public:
    /**
     * @brief 构造函数
     * @param ptr 初始指针值，默认为nullptr
     */
    explicit AtomicPointer(T* ptr = nullptr) noexcept;

    // 禁用拷贝
    AtomicPointer(const AtomicPointer&) = delete;
    AtomicPointer& operator=(const AtomicPointer&) = delete;

    /**
     * @brief 原子加载指针
     * @return 当前指针值
     */
    T* load() const noexcept;

    /**
     * @brief 原子存储指针
     * @param new_ptr 新的指针值
     */
    void store(T* new_ptr) noexcept;

    /**
     * @brief 原子交换指针
     * @param new_ptr 新的指针值
     * @return 原来的旧指针
     */
    T* exchange(T* new_ptr) noexcept;

    /**
     * @brief 指针CAS操作
     * @param expected 期望的旧指针（传入传出）
     * @param desired 新的指针
     * @return true 交换成功，false 失败
     */
    bool compare_exchange(T*& expected, T* desired) noexcept;

    /**
     * @brief 自动转换为指针类型
     */
    operator T*() const noexcept;

    /**
     * @brief 箭头运算符，用于访问指针指向的对象
     */
    T* operator->() const noexcept;

private:
    std::atomic<T*> ptr_;  // 底层原子指针
};

// 第三节：基本互斥锁
/**
 * @brief Mutex 类 - 基本互斥锁（增强版）
 *
 * 什么是互斥锁？
 * - 同一时刻只有一个线程能持有
 * - 其他想获取锁的线程会阻塞等待
 * - 直到锁被释放，其他线程才能继续
 *
 * std::mutex vs 增强版Mutex：
 * - std::mutex 只有 lock() 和 try_lock()
 * - 增强版增加了 try_lock_for() 和 try_lock_until()
 * - 这两个方法支持超时，避免无限等待
 *
 * 为什么要支持超时？
 * - 避免死锁：如果锁被永久持有，超时后可以放弃
 * - 提高响应性：等待一定时间后可以做其他事
 *
 * 示例：
 * @code
 *   Mutex mutex;
 *
 *   // 方式1：无限等待
 *   mutex.lock();
 *   // 临界区
 *   mutex.unlock();
 *
 *   // 方式2：尝试获取，超时放弃
 *   if (mutex.try_lock_for(std::chrono::milliseconds(100))) {
 *       // 获取成功
 *       // 临界区
 *       mutex.unlock();
 *   } else {
 *       // 获取失败，超时
 *   }
 * @endcode
 */
class Mutex {
public:
    /**
     * @brief 构造函数
     *
     * 初始化互斥锁为未锁定状态
     */
    Mutex();

    /**
     * @brief 析构函数
     */
    ~Mutex() = default;

    // 禁用拷贝
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /**
     * @brief 获取锁（阻塞直到获取）
     *
     * 如果锁已被占用，调用线程会阻塞（挂起）
     * 直到锁被释放，线程才会被唤醒继续执行
     *
     * 注意：同一线程不能连续两次 lock()，会死锁！
     * （如果需要同一线程多次获取，用 RecursiveMutex）
     */
    void lock();

    /**
     * @brief 尝试获取锁（非阻塞）
     * @return true 获取成功，false 锁已被占用
     *
     * 不会阻塞，立即返回
     * 即使失败也不会等待
     */
    bool try_lock();

    /**
     * @brief 尝试获取锁，带超时
     * @param duration 超时时长
     * @return true 获取成功，false 超时未获取
     *
     * 示例：
     * @code
     *   if (mutex.try_lock_for(std::chrono::seconds(1))) {
     *       // 获取成功
     *   }
     * @endcode
     */
    template<typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& duration);

    /**
     * @brief 尝试获取锁，到绝对时间点
     * @param time_point 截止时间点
     * @return true 获取成功，false 超时
     *
     * 示例：
     * @code
     *   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
     *   if (mutex.try_lock_until(deadline)) {
     *       // 获取成功
     *   }
     * @endcode
     */
    template<typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& time_point);

    /**
     * @brief 释放锁
     *
     * 唤醒一个正在等待该锁的线程（如果有）
     * 如果没有等待线程，什么也不做
     */
    void unlock();

private:
    std::mutex mutex_;              // 底层 std::mutex
    std::condition_variable cv_;     // 条件变量，用于 wait/notify
    bool locked_ = false;           // 锁状态标志
};


// 第四节：自旋锁
/**
 * @brief SpinLock 类 - 自旋锁（增强版）
 *
 * 什么是自旋锁？
 * - 获取锁失败时，不是阻塞挂起，而是循环等待
 * - 一直 while 循环检查锁是否被释放
 * - 适合临界区极短（< 100条指令）的场景
 *
 * SpinLock vs Mutex：
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │                  SpinLock              │           Mutex               │
 * ├────────────────────────────────────────────────────────────────────────┤
 * │ 获取失败时：自旋等待（消耗CPU）               │获取失败时：挂起（不消耗CPU）   │
 * │ 临界区极短时性能高                           │临界区较长时性能高            │
 * │ 避免上下文切换开销                           │上下文切换开销大              │
 * │ 单核CPU效果差（浪费CPU）                     │单核/多核都适用               │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * 版本2的增强：
 * 1. 使用 CPU_PAUSE() 指令（比空循环更省电）
 * 2. 指数退避策略：等待次数指数增长，避免惊群效应
 * 3. 超过最大自旋次数后让出CPU（yield）
 *
 * 什么是惊群效应？
 * - 多个线程同时等待同一把锁
 * - 锁释放时，所有线程都被唤醒
 * - 但只有1个线程能获取锁，其他线程又得等待/自旋
 * - 造成大量无用唤醒
 * - 退避策略让线程在不同时间醒来，减少惊群
 *
 * alignas(64) 的作用：
 * - 保证 flag_ 位于独立的CPU缓存行
 * - 避免"伪共享"问题
 * - 64字节是大多数CPU缓存行大小
 *
 * 示例：
 * @code
 *   SpinLock spinlock;
 *
 *   // 临界区代码执行很快（几条指令）
 *   LockGuard<SpinLock> guard(spinlock);
 *   // 操作共享数据
 * @endcode
 */
class SpinLock {
public:
    /**
     * @brief 构造函数
     *
     * 初始化 atomic_flag 为清除状态（未锁定）
     */
    SpinLock();

    /**
     * @brief 析构函数
     */
    ~SpinLock() = default;

    // 禁用拷贝
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    /**
     * @brief 获取锁（自旋等待）
     *
     * 实现细节：
     * 1. 先尝试一次获取（大多数情况下能成功）
     * 2. 如果失败，进入退避循环
     * 3. 指数退避：1次 → 2次 → 4次 → ... → 64次
     * 4. 超过最大自旋次数后，调用 yield() 让出CPU
     */
    void lock();

    /**
     * @brief 尝试获取锁
     * @return true 成功，false 失败
     *
     * 使用 compare_exchange_strong 保证原子性
     */
    bool try_lock();

    /**
     * @brief 释放锁
     */
    void unlock();

private:
    // alignas(64) 确保 flag_ 独立占一个缓存行
    // 避免伪共享（false sharing）问题
    alignas(64) std::atomic_flag flag_;
};

/**
 * @brief RecursiveMutex 类 - 递归互斥锁
 *
 * 什么是递归锁？
 * - 同一线程可以多次获取同一把锁而不会死锁
 * - 每次 lock() 匹配一次 unlock()
 * - 锁的持有计数 +1
 *
 * 为什么要用递归锁？
 * - 递归函数需要访问共享数据
 * - 外层函数已经获取锁，调用内层递归函数时又要获取锁
 * - 普通 mutex 会死锁，递归锁不会
 *
 * 缺点：
 * - 比普通 mutex 性能稍差
 * - 容易滥用，不推荐优先使用
 *
 * 示例：
 * @code
 *   RecursiveMutex rmutex;
 *
 *   void recursive_function(int n) {
 *       LockGuard<RecursiveMutex> guard(rmutex);
 *       if (n > 0) {
 *           // 做点什么
 *           recursive_function(n - 1);  // 可以再次获取锁
 *       }
 *   }
 *
 *   // 主线程调用
 *   recursive_function(10);  // 正常工作
 * @endcode
 */
class RecursiveMutex {
public:
    /**
     * @brief 构造函数
     */
    RecursiveMutex();

    /**
     * @brief 析构函数
     */
    ~RecursiveMutex() = default;

    // 禁用拷贝
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    /**
     * @brief 获取锁
     *
     * 实现逻辑：
     * 1. 检查是否是同一个线程持有
     * 2. 如果是同一线程，计数+1，直接返回
     * 3. 如果不是，阻塞等待直到锁可用
     */
    void lock();

    /**
     * @brief 尝试获取锁
     * @return true 成功，false 失败（其他线程持有）
     */
    bool try_lock();

    /**
     * @brief 释放锁
     *
     * 实现逻辑：
     * 1. 检查是否是持有线程在调用
     * 2. 计数-1
     * 3. 如果计数变为0，完全释放锁，唤醒等待线程
     */
    void unlock();

private:
    std::mutex mutex_;                  // 底层互斥锁
    std::condition_variable cv_;         // 条件变量
    std::thread::id owner_thread_;      // 当前持有线程ID
    int count_ = 0;                     // 递归计数
};

// 第五节：读写锁
/**
 * @brief RWLock 类 - 读写锁
 *
 * 什么是读写锁？
 * - 读操作（不修改数据）可以多个线程同时进行
 * - 写操作（修改数据）必须独占，其他线程不能读或写
 *
 * 读写锁的适用场景：
 * - 读多写少：1000次读，1次写
 * - 读操作耗时：需要访问大量数据
 * - 写操作不频繁：修改次数少
 *
 * 为什么读写锁效率高？
 * - 假设用普通互斥锁：1000次读 + 1次写 = 1001次串行执行
 * - 用读写锁：1000次读可以并行（同一时刻），1次写串行
 * - 理论上性能提升 ~1000倍（读操作）
 *
 * std::shared_mutex（C++17）读写锁原理：
 * - lock() / unlock() = 写锁
 * - lock_shared() / unlock_shared() = 读锁
 * - 写锁是独占的（其他读写都无法同时）
 * - 读锁是共享的（其他读锁可以同时）
 *
 * 示例：
 * @code
 *   RWLock rwlock;
 *   std::string cache;
 *
 *   // 多个线程可以同时读取
 *   void read_data() {
 *       ReadLockGuard<RWLock> guard(rwlock);  // 读锁
 *       std::string data = cache;  // 安全读取
 *   }
 *
 *   // 只有一个线程能写入
 *   void write_data(const std::string& new_data) {
 *       WriteLockGuard<RWLock> guard(rwlock);  // 写锁
 *       cache = new_data;  // 安全写入
 *   }
 * @endcode
 */
class RWLock {
public:
    /**
     * @brief 构造函数
     */
    RWLock();

    /**
     * @brief 析构函数
     */
    ~RWLock() = default;

    // 禁用拷贝
    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;

    /**
     * @brief 获取读锁（共享锁）
     *
     * 多个线程可以同时持有读锁
     * 只要没有线程持有写锁
     */
    void read_lock();

    /**
     * @brief 释放读锁
     */
    void read_unlock();

    /**
     * @brief 获取写锁（独占锁）
     *
     * 只有没有任何读写锁时才能获取
     * 获取后，其他任何线程都不能读或写
     */
    void write_lock();

    /**
     * @brief 释放写锁
     */
    void write_unlock();

    /**
     * @brief 尝试获取读锁（非阻塞）
     * @return true 成功，false 失败（可能有写锁）
     */
    bool try_read_lock();

    /**
     * @brief 尝试获取写锁（非阻塞）
     * @return true 成功，false 失败（可能有读锁或写锁）
     */
    bool try_write_lock();

    /**
     * @brief 尝试获取读锁，带超时
     */
    template<typename Rep, typename Period>
    bool try_read_lock_for(const std::chrono::duration<Rep, Period>& duration);

    /**
     * @brief 尝试获取写锁，带超时
     */
    template<typename Rep, typename Period>
    bool try_write_lock_for(const std::chrono::duration<Rep, Period>& duration);

private:
    std::shared_mutex mutex_;  // C++17 读写锁
};

/**
 * @brief RWLock2 类 - 改进的读写锁（支持写优先）
 *
 * 相对于 RWLock 的改进：
 * - 支持写锁优先，避免写饥饿
 * - 有写锁等待时，新来的读锁请求会阻塞
 * - 保证写锁最终一定能获取到
 *
 * 什么是写饥饿？
 * - 如果一直有新读锁到来，写锁可能永远获取不到
 * - RWLock2 会阻塞新读锁，让写锁先执行
 *
 * 示例：
 * @code
 *   RWLock2 rwlock2;
 *
 *   // 线程A、B、C在等待写
 *   // 新来的读请求会等待，直到写锁全部完成
 *   void reader() {
 *       ReadLockGuard<RWLock2> guard(rwlock2);  // 可能会等
 *       // 读数据
 *   }
 *
 *   void writer() {
 *       WriteLockGuard<RWLock2> guard(rwlock2);  // 优先执行
 *       // 写数据
 *   }
 * @endcode
 */
class RWLock2 {
public:
    /**
     * @brief 构造函数
     */
    RWLock2();

    /**
     * @brief 析构函数
     */
    ~RWLock2() = default;

    // 禁用拷贝
    RWLock2(const RWLock2&) = delete;
    RWLock2& operator=(const RWLock2&) = delete;

    /**
     * @brief 获取读锁
     *
     * 实现逻辑：
     * - 如果有写锁等待，阻塞新读请求（写优先）
     * - 如果没有写锁等待，获取读锁
     */
    void read_lock();

    /**
     * @brief 释放读锁
     */
    void read_unlock();

    /**
     * @brief 获取写锁
     */
    void write_lock();

    /**
     * @brief 释放写锁
     */
    void write_unlock();

    /**
     * @brief 尝试获取读锁
     */
    bool try_read_lock();

    /**
     * @brief 尝试获取写锁
     */
    bool try_write_lock();

    /**
     * @brief 查询当前有多少读锁
     */
    int num_readers() const;

    /**
     * @brief 查询是否有写锁活动
     */
    bool has_writer_active() const;

    /**
     * @brief 查询有多少写锁在等待
     */
    int num_writers_waiting() const;

private:
    mutable std::mutex mutex_;         // 保护所有成员变量
    std::condition_variable cv_;       // 条件变量
    int readers_ = 0;                   // 当前读锁数量
    int writers_waiting_ = 0;          // 等待中的写锁数量
    bool writer_active_ = false;       // 是否有写锁活动
};

// 第六节：RAII锁守卫模板
/**
 * @brief LockGuard 模板类 - RAII锁守卫
 *
 * 什么是RAII？
 * - Resource Acquisition Is Initialization
 * - 资源获取即初始化
 * - 构造函数获取资源，析构函数释放资源
 *
 * 为什么需要锁守卫？
 * ┌─────────────────────────────────────────────────────────────┐
 * // 问题：忘记 unlock() 导致死锁                               │
 *   mutex.lock();                                              │
 *   if (something) {                                           │
 *       return;  // 忘记 unlock！                              │
 *   }                                                          │
 *   mutex.unlock();                                            │
 * └─────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * // 解决：LockGuard 自动管理                                   │
 *   {                                                          │
 *       LockGuard<Mutex> guard(mutex);  // 构造函数加锁        │
 *       if (something) {                                       │
 *           return;  // guard析构，自动解锁                     │
 *       }                                                      │
 *   }  // guard析构，自动解锁                                   │
 * └─────────────────────────────────────────────────────────────┘
 *
 * LockGuard 的特点：
 * - 构造函数获取锁
 * - 析构函数释放锁
 * - 无论函数如何退出（正常return、异常、goto），都会解锁
 * - 防止忘记 unlock() 导致的死锁
 *
 * @tparam Lockable 必须是支持 lock() 和 unlock() 的锁类型
 */
template<typename Lockable>
class LockGuard {
public:
    /**
     * @brief 构造函数，获取锁
     * @param lock 锁的引用
     *
     * 使用 explicit 防止隐式转换
     * 必须传入一个已存在的锁引用，不能是 nullptr
     */
    explicit LockGuard(Lockable& lock);

    /**
     * @brief 析构函数，释放锁
     *
     * 只有在仍然持有锁时才释放
     * 如果提前调用了 unlock()，就不重复释放
     */
    ~LockGuard();

    // 禁用拷贝
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

    /**
     * @brief 提前释放锁
     *
     * 用于需要提前解锁的场景
     * 调用后 LockGuard 不再拥有锁
     * 析构时不会重复释放
     */
    void unlock();

    /**
     * @brief 检查是否持有锁
     */
    [[nodiscard]] bool owns_lock() const;

    /**
     * @brief 转换为 bool
     *
     * 方便检查是否获取成功
     * if (LockGuard<Mutex> guard(mutex)) { ... }
     */
    explicit operator bool() const;

private:
    Lockable& lock_;   // 锁的引用
    bool owned_;       // 是否持有锁（防止重复释放）
};

/**
 * @brief TryLockGuard 模板类 - try_lock的RAII封装
 *
 * 用于 try_lock 场景：
 * - 尝试获取锁，如果失败也不阻塞
 * - 获取成功后自动释放
 *
 * 示例：
 * @code
 *   Mutex mutex;
 *
 *   // 尝试获取锁，不等待
 *   TryLockGuard<Mutex> guard(mutex);
 *   if (guard) {
 *       // 获取成功
 *   } else {
 *       // 获取失败，但不会有任何问题
 *   }
 * @endcode
 */
template<typename Lockable>
class TryLockGuard {
public:
    /**
     * @brief 构造函数，尝试获取锁
     * @param lock 锁的引用
     *
     * 调用 lock.try_lock()
     * 如果成功，owned_ = true
     * 如果失败，owned_ = false
     */
    explicit TryLockGuard(Lockable& lock);

    /**
     * @brief 析构函数，如果持有锁则释放
     */
    ~TryLockGuard();

    // 禁用拷贝
    TryLockGuard(const TryLockGuard&) = delete;
    TryLockGuard& operator=(const TryLockGuard&) = delete;

    /**
     * @brief 检查是否成功获取锁
     */
    [[nodiscard]] bool owns_lock() const;

    /**
     * @brief 转换为 bool
     */
    explicit operator bool() const;

    /**
     * @brief 提前释放锁
     */
    void unlock();

private:
    Lockable& lock_;
    bool owned_;
};

/**
 * @brief ReadLockGuard 模板类 - 读锁的RAII封装
 *
 * 专门用于 RWLock/RWLock2 的读锁管理
 * 析构时调用 read_unlock()
 *
 * @tparam Lockable 必须是支持 read_lock() 和 read_unlock() 的锁类型
 */
template<typename Lockable>
class ReadLockGuard {
public:
    /**
     * @brief 构造函数，获取读锁
     */
    explicit ReadLockGuard(Lockable& lock);

    /**
     * @brief 析构函数，释放读锁
     */
    ~ReadLockGuard();

    // 禁用拷贝
    ReadLockGuard(const ReadLockGuard&) = delete;
    ReadLockGuard& operator=(const ReadLockGuard&) = delete;

    /**
     * @brief 提前释放读锁
     */
    void unlock();

    /**
     * @brief 检查是否持有锁
     */
    [[nodiscard]] bool owns_lock() const;

    /**
     * @brief 转换为 bool
     */
    explicit operator bool() const;

private:
    Lockable& lock_;
    bool owned_;
};

/**
 * @brief WriteLockGuard 模板类 - 写锁的RAII封装
 *
 * 专门用于 RWLock/RWLock2 的写锁管理
 * 析构时调用 write_unlock()
 *
 * @tparam Lockable 必须是支持 write_lock() 和 write_unlock() 的锁类型
 */
template<typename Lockable>
class WriteLockGuard {
public:
    /**
     * @brief 构造函数，获取写锁
     */
    explicit WriteLockGuard(Lockable& lock);

    /**
     * @brief 析构函数，释放写锁
     */
    ~WriteLockGuard();

    // 禁用拷贝
    WriteLockGuard(const WriteLockGuard&) = delete;
    WriteLockGuard& operator=(const WriteLockGuard&) = delete;

    /**
     * @brief 提前释放写锁
     */
    void unlock();

    /**
     * @brief 检查是否持有锁
     */
    [[nodiscard]] bool owns_lock() const;

    /**
     * @brief 转换为 bool
     */
    explicit operator bool() const;

private:
    Lockable& lock_;
    bool owned_;
};


// 第七节：同步原语


/**
 * @brief Semaphore 类 - 信号量
 *
 * 什么是信号量？
 * - 一种更通用的同步原语
 * - 有一个计数器，初始化为某个值
 * - wait() 使计数器-1，如果为0则阻塞
 * - post() 使计数器+1，唤醒等待线程
 *
 * 信号量 vs 互斥锁：
 * - 互斥锁：计数器只能是0或1（二值信号量）
 * - 信号量：计数器可以是任意非负整数
 *
 * 信号量的应用场景：
 * 1. 控制同时访问的线程数量
 *    - 比如：限制同时只有3个线程访问数据库
 *    - Semaphore sem(3);
 *    - 每个线程：sem.wait() → 访问数据库 → sem.post()
 *
 * 2. 生产者-消费者
 *    - empty 表示空槽数量
 *    - full 表示满槽数量
 *
 * 示例：
 * @code
 *   Semaphore sem(2);  // 最多2个线程同时访问
 *
 *   // 线程A
 *   sem.wait();  // 计数从2变成1
 *   // 访问共享资源
 *   sem.post();  // 计数从1变成2
 *
 *   // 线程B
 *   sem.wait();  // 计数从2变成1
 *   // 访问共享资源
 *   sem.post();  // 计数从1变成2
 *
 *   // 线程C
 *   sem.wait();  // 计数从2变成1（还能进入）
 *   // ...
 * @endcode
 */
class Semaphore {
public:
    /**
     * @brief 构造函数
     * @param initial_count 初始计数
     */
    explicit Semaphore(int initial_count = 0);

    /**
     * @brief 析构函数
     */
    ~Semaphore() = default;

    // 禁用拷贝
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /**
     * @brief 等待（P操作）
     *
     * 计数-1
     * 如果计数为0，阻塞等待直到计数>0
     */
    void wait();

    /**
     * @brief 尝试等待（非阻塞）
     * @return true 成功减计数，false 计数已经为0
     */
    bool try_wait();

    /**
     * @brief 等待，带超时
     * @param duration 超时时长
     * @return true 成功，false 超时
     */
    template<typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& duration);

    /**
     * @brief 释放（V操作）
     *
     * 计数+1
     * 唤醒一个等待线程（如果有）
     */
    void post();

    /**
     * @brief 全部释放
     *
     * 计数设为最大值
     * 唤醒所有等待线程
     */
    void post_all();

    /**
     * @brief 获取当前计数
     */
    int count() const;

private:
    mutable std::mutex mutex_;         // 保护计数
    std::condition_variable cv_;        // 条件变量
    int count_;                        // 当前计数
    int max_count_;                    // 最大计数（初始值）
};

/**
 * @brief CountDownLatch 类 - 倒计时门栓
 *
 * 什么是倒计时门栓？
 * - 一种线程同步机制
 * - 初始化时设置一个计数 N
 * - 每调用一次 count_down()，计数-1
 * - wait() 阻塞直到计数变为0
 *
 * 倒计时门栓 vs 信号量：
 * - 信号量：计数可以重复使用（用完可以再加）
 * - 倒计时门栓：计数只能减少到0，不能重置（除非重建）
 *
 * 应用场景：
 * 1. 等待N个线程完成初始化
 *    - N个线程各自完成初始化后调用 count_down()
 *    - 主线程调用 wait() 等待全部完成
 *
 * 2. 等待某个操作完成
 *    - 比如：等待3个worker线程都启动完成
 *
 * 示例：
 * @code
 *   CountDownLatch latch(3);  // 3个线程
 *
 *   // 主线程等待
 *   latch.wait();  // 阻塞，直到计数变为0
 *   std::cout << "所有线程完成！\n";
 *
 *   // 线程函数
 *   void worker() {
 *       // 做工作...
 *       latch.count_down();  // 计数-1
 *   }
 *
 *   // 创建3个线程
 *   std::thread t1(worker);
 *   std::thread t2(worker);
 *   std::thread t3(worker);
 *
 *   t1.join();
 *   t2.join();
 *   t3.join();
 * @endcode
 */
class CountDownLatch {
public:
    /**
     * @brief 构造函数
     * @param count 初始计数
     */
    explicit CountDownLatch(int count);

    /**
     * @brief 析构函数
     */
    ~CountDownLatch() = default;

    // 禁用拷贝
    CountDownLatch(const CountDownLatch&) = delete;
    CountDownLatch& operator=(const CountDownLatch&) = delete;

    /**
     * @brief 等待计数变为0
     *
     * 如果计数已经是0，立即返回
     * 否则阻塞，直到计数变为0
     */
    void wait();

    /**
     * @brief 等待，带超时
     * @param duration 超时时长
     * @return true 计数已变为0，false 超时
     */
    template<typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& duration);

    /**
     * @brief 计数-1
     *
     * 如果计数已经为0，什么也不做
     * 如果减到0，唤醒所有等待线程
     */
    void count_down();

    /**
     * @brief 获取当前计数
     */
    int count() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
};

/**
 * @brief CyclicBarrier 类 - 循环屏障
 *
 * 什么是循环屏障？
 * - N个线程都到达屏障点后，才能一起继续执行
 * - 可以重置后重复使用（"循环"的意思）
 *
 * CountDownLatch vs CyclicBarrier：
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │                    CountDownLatch          │      CyclicBarrier        │
 * ├────────────────────────────────────────────────────────────────────────┤
 * │ 计数只能减少到0，不能重置              │ 可以重置，重复使用           │
 * │ 一旦为0，就结束了                      │ 所有线程通过后，可以再次使用  │
 * │ 一般用于"等待N个线程完成"              │ 一般用于"N个线程互相等待"    │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * 应用场景：
 * 1. N个线程需要互相等待，然后同时开始下一步
 *    - 比如：多线程排序，每轮排序后等待其他线程完成，然后合并
 *
 * 2. 模拟"起跑"：所有运动员等待发令枪，然后一起跑
 *
 * 示例：
 * @code
 *   CyclicBarrier barrier(3);  // 3个线程
 *
 *   void worker(int id) {
 *       // 第一阶段：各自准备
 *       prepare();
 *
 *       // 到达屏障点，等待其他线程
 *       barrier.wait();  // 3个线程都会在这里等待
 *
 *       // 所有线程都到达后，一起继续执行
 *       // 第二阶段：同时开始执行任务
 *       execute();
 *   }
 *
 *   // 如果需要重置：
 *   barrier.reset();  // 可以让所有线程重新开始
 * @endcode
 */
class CyclicBarrier {
public:
    /**
     * @brief 构造函数
     * @param parties 参与线程数
     */
    explicit CyclicBarrier(int parties);

    /**
     * @brief 析构函数
     */
    ~CyclicBarrier() = default;

    // 禁用拷贝
    CyclicBarrier(const CyclicBarrier&) = delete;
    CyclicBarrier& operator=(const CyclicBarrier&) = delete;

    /**
     * @brief 等待其他线程
     * @return 返回到达序号（0~parties-1），用于区分线程
     *
     * 实现逻辑：
     * 1. 计数-1
     * 2. 如果计数>0，等待
     * 3. 如果计数==0，重置计数，唤醒所有等待线程
     */
    int wait();

    /**
     * @brief 重置屏障
     *
     * 所有等待中的线程会立即返回（Generation已更新）
     * 计数重置为初始值
     */
    void reset();

    /**
     * @brief 返回参与线程数
     */
    int parties() const;

    /**
     * @brief 返回当前正在等待的线程数
     */
    int waiting() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int parties_;       // 参与线程数（不变）
    int count_;         // 当前计数
    int generation_;    // 代数（用于区分reset前后的等待）
};


// 第八节：分片锁


/**
 * @brief ShardedLock 类 - 分片锁数组
 *
 * 什么是分片锁？
 * - 把一把锁分散成多个分片锁
 * - 减少锁竞争，提高并发度
 *
 * 为什么需要分片锁？
 * - 如果全局只有一把锁，所有线程都要竞争同一把锁
 * - 如果分成N个分片，线程访问不同数据时，竞争不同的锁
 * - 大大减少锁竞争
 *
 * 工作原理：
 * ┌─────────────────────────────────────────────────────────────┐
 * // 全局锁（不好）                                             │
 * // 所有数据共用一把锁                                         │
 * // 线程A访问数据1，线程B访问数据2 → 也要竞争同一把锁          │
 * └─────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * // 分片锁（好）                                               │
 * // 数据按key哈希到不同分片，每个分片独立锁                     │
 * // 线程A访问key1 → 分片0 → 锁0                               │
 * // 线程B访问key2 → 分片1 → 锁1                               │
 * // 两个线程完全并行，互不干扰！                               │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 示例：
 * @code
 *   // 假设有10000个key需要访问
 *   // 不分片：所有访问都要竞争同一把锁
 *   // 分片16：平均每个分片只有625个key，锁竞争大大减少
 *
 *   ShardedLock<SpinLock> sharded_locks(16);  // 16个分片
 *
 *   void access(const std::string& key) {
 *       // 根据key的哈希值选择分片
 *       auto& lock = sharded_locks.get_shard(key);
 *
 *       // 这个分片的锁只保护这个分片的数据
 *       SpinLockGuard guard(lock);
 *       // 访问该分片的数据
 *   }
 * @endcode
 *
 * @tparam LockType 分片锁的类型（SpinLock、Mutex等）
 */
template<typename LockType>
class ShardedLock {
public:
    /**
     * @brief 构造函数
     * @param num_shards 分片数量
     *
     * 建议分片数 = CPU核心数 * 2
     * 比如8核CPU建议用16分片
     */
    explicit ShardedLock(size_t num_shards);

    // 禁用拷贝
    ShardedLock(const ShardedLock&) = delete;
    ShardedLock& operator=(const ShardedLock&) = delete;

    /**
     * @brief 根据key获取对应的分片锁
     * @param key 数据的key
     * @return 该key对应的分片锁引用
     *
     * 使用string的哈希值选择分片
     * 相同的key总是映射到相同的分片
     */
    LockType& get_shard(const std::string& key);

    /**
     * @brief 根据哈希值获取分片锁
     * @param hash 哈希值
     * @return 对应分片的锁引用
     */
    LockType& get_shard(size_t hash);

    /**
     * @brief 返回分片总数
     */
    [[nodiscard]] size_t num_shards() const;

    /**
     * @brief 返回所有分片的引用
     *
     * 高级用法：遍历所有分片
     */
    std::vector<LockType>& shards();

private:
    std::vector<LockType> shards_;  // 分片数组
    size_t num_shards_;             // 分片数量
};

/**
 * @brief ShardedRWLock 类 - 分片读写锁数组
 *
 * 组合了分片锁和读写锁的优点：
 * - 分片减少锁竞争
 * - 读写锁允许读并发
 *
 * 适用场景：
 * - 大量数据需要分片管理
 * - 读多写少
 *
 * 示例：
 * @code
 *   ShardedRWLock cache(16);  // 16个分片
 *
 *   // 读取数据（多个线程可并发读同一分片）
 *   std::string read(const std::string& key) {
 *       auto& lock = cache.get_shard(key);
 *       RWLockReadGuard guard(lock);  // 读锁
 *       return find_in_shard(key);
 *   }
 *
 *   // 写入数据（独占）
 *   void write(const std::string& key, const std::string& value) {
 *       auto& lock = cache.get_shard(key);
 *       RWLockWriteGuard guard(lock);  // 写锁
 *       update_shard(key, value);
 *   }
 * @endcode
 */
class ShardedRWLock {
public:
    /**
     * @brief 构造函数
     * @param num_shards 分片数量
     */
    explicit ShardedRWLock(size_t num_shards);

    // 禁用拷贝
    ShardedRWLock(const ShardedRWLock&) = delete;
    ShardedRWLock& operator=(const ShardedRWLock&) = delete;

    /**
     * @brief 根据key获取分片读写锁
     */
    RWLock& get_shard(const std::string& key);

    /**
     * @brief 根据哈希值获取分片读写锁
     */
    RWLock& get_shard(size_t hash);

    /**
     * @brief 返回分片总数
     */
    [[nodiscard]] size_t num_shards() const;

    /**
     * @brief 返回所有分片
     */
    std::vector<RWLock>& shards();

private:
    std::vector<RWLock> shards_;
    size_t num_shards_;
};


// 第九节：便利类型别名


/**
 * @brief 便利类型别名 - 让代码更简洁
 *
 * 使用示例：
 * @code
 *   Mutex mutex;
 *   MutexGuard guard(mutex);  // 代替 LockGuard<Mutex>
 *
 *   SpinLock spinlock;
 *   SpinLockGuard guard2(spinlock);  // 代替 LockGuard<SpinLock>
 *
 *   RWLock rwlock;
 *   RWLockReadGuard reader(rwlock);  // 代替 ReadLockGuard<RWLock>
 *   RWLockWriteGuard writer(rwlock);  // 代替 WriteLockGuard<RWLock>
 * @endcode
 */

// Mutex 的 RAII 封装
using MutexGuard = LockGuard<Mutex>;

// SpinLock 的 RAII 封装
using SpinLockGuard = LockGuard<SpinLock>;

// RecursiveMutex 的 RAII 封装
using RecursiveMutexGuard = LockGuard<RecursiveMutex>;

// RWLock 读锁的 RAII 封装
using RWLockReadGuard = ReadLockGuard<RWLock>;

// RWLock 写锁的 RAII 封装
using RWLockWriteGuard = WriteLockGuard<RWLock>;

// RWLock2 读锁的 RAII 封装
using RWLock2ReadGuard = ReadLockGuard<RWLock2>;

// RWLock2 写锁的 RAII 封装
using RWLock2WriteGuard = WriteLockGuard<RWLock2>;

}  // namespace cc_server

#endif  // CONCURRENTCACHE_LOCK_H
