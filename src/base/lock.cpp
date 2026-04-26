#include "lock.h"

namespace cc_server {

// ============================================================
// AtomicInteger 实现
// ============================================================
AtomicInteger::AtomicInteger(int initial_value) noexcept : value_(initial_value) {}

int AtomicInteger::load() const noexcept {
    return value_.load(std::memory_order_acquire);
}

void AtomicInteger::store(int val) noexcept {
    value_.store(val, std::memory_order_release);
}

int AtomicInteger::exchange(int new_val) noexcept {
    return value_.exchange(new_val, std::memory_order_acq_rel);
}

bool AtomicInteger::compare_exchange(int& expected, int desired) noexcept {
    return value_.compare_exchange_weak(
        expected, desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool AtomicInteger::compare_exchange_strong(int& expected, int desired) noexcept {
    return value_.compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

int AtomicInteger::fetch_add(int arg) noexcept {
    return value_.fetch_add(arg, std::memory_order_acq_rel);
}

int AtomicInteger::fetch_sub(int arg) noexcept {
    return value_.fetch_sub(arg, std::memory_order_acq_rel);
}

int AtomicInteger::fetch_and(int arg) noexcept {
    return value_.fetch_and(arg, std::memory_order_acq_rel);
}

int AtomicInteger::fetch_or(int arg) noexcept {
    return value_.fetch_or(arg, std::memory_order_acq_rel);
}

int AtomicInteger::fetch_xor(int arg) noexcept {
    return value_.fetch_xor(arg, std::memory_order_acq_rel);
}

int AtomicInteger::operator++() noexcept {
    return fetch_add(1) + 1;
}

int AtomicInteger::operator++(int) noexcept {
    return fetch_add(1);
}

int AtomicInteger::operator--() noexcept {
    return fetch_sub(1) - 1;
}

int AtomicInteger::operator--(int) noexcept {
    return fetch_sub(1);
}

int AtomicInteger::operator+=(int arg) noexcept {
    return fetch_add(arg) + arg;
}

int AtomicInteger::operator-=(int arg) noexcept {
    return fetch_sub(arg) - arg;
}

AtomicInteger::operator int() const noexcept {
    return load();
}

// ============================================================
// AtomicPointer 实现
// ============================================================
template<typename T>
AtomicPointer<T>::AtomicPointer(T* ptr) noexcept : ptr_(ptr) {}

template<typename T>
T* AtomicPointer<T>::load() const noexcept {
    return ptr_.load(std::memory_order_acquire);
}

template<typename T>
void AtomicPointer<T>::store(T* new_ptr) noexcept {
    ptr_.store(new_ptr, std::memory_order_release);
}

template<typename T>
T* AtomicPointer<T>::exchange(T* new_ptr) noexcept {
    return ptr_.exchange(new_ptr, std::memory_order_acq_rel);
}

template<typename T>
bool AtomicPointer<T>::compare_exchange(T*& expected, T* desired) noexcept {
    return ptr_.compare_exchange_weak(
        expected, desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

template<typename T>
AtomicPointer<T>::operator T*() const noexcept {
    return load();
}

template<typename T>
T* AtomicPointer<T>::operator->() const noexcept {
    return load();
}

// 显式实例化常用类型
template class AtomicPointer<void>;
template class AtomicPointer<char>;

// ============================================================
// Mutex 实现
// ============================================================
Mutex::Mutex() = default;

void Mutex::lock() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !locked_; });
    locked_ = true;
}

bool Mutex::try_lock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (locked_) {
        return false;
    }
    locked_ = true;
    return true;
}

template<typename Rep, typename Period>
bool Mutex::try_lock_for(const std::chrono::duration<Rep, Period>& duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, duration, [this] { return !locked_; })) {
        return false;
    }
    locked_ = true;
    return true;
}

template<typename Clock, typename Duration>
bool Mutex::try_lock_until(const std::chrono::time_point<Clock, Duration>& time_point) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_until(lock, time_point, [this] { return !locked_; })) {
        return false;
    }
    locked_ = true;
    return true;
}

void Mutex::unlock() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked_ = false;
    }
    cv_.notify_one();
}

// ============================================================
// SpinLock 实现
// ============================================================
SpinLock::SpinLock() = default;

void SpinLock::lock() {
    // 首先尝试一次获取锁（最常见情况）
    if (try_lock()) {
        return;
    }

    // 退避策略参数
    int spin_count = 1;
    constexpr int max_spins_before_yield = 1000;

    // 指数退避循环
    do {
        for (int i = 0; i < spin_count; ++i) {
            CPU_PAUSE();
        }
        if (try_lock()) {
            return;
        }
        // 指数增加等待次数，上限64
        spin_count = std::min(spin_count * 2, 64);
    } while (spin_count < max_spins_before_yield);

    // 超过最大自旋次数，让出CPU
    std::this_thread::yield();
}

bool SpinLock::try_lock() {
    return !flag_.test_and_set(std::memory_order_acquire);
}

void SpinLock::unlock() {
    flag_.clear(std::memory_order_release);
}

// ============================================================
// RecursiveMutex 实现
// ============================================================
RecursiveMutex::RecursiveMutex() = default;

void RecursiveMutex::lock() {
    std::thread::id this_thread = std::this_thread::get_id();
    if (owner_thread_ == this_thread) {
        count_++;
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this, this_thread] {
        return owner_thread_ != this_thread;
    });
    owner_thread_ = this_thread;
    count_ = 1;
}

bool RecursiveMutex::try_lock() {
    std::thread::id this_thread = std::this_thread::get_id();
    if (owner_thread_ == this_thread) {
        count_++;
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_thread_ != std::thread::id()) {
        return false;
    }
    owner_thread_ = this_thread;
    count_ = 1;
    return true;
}

void RecursiveMutex::unlock() {
    std::thread::id this_thread = std::this_thread::get_id();
    if (owner_thread_ != this_thread) {
        return;
    }
    count_--;
    if (count_ == 0) {
        owner_thread_ = std::thread::id();
        cv_.notify_one();
    }
}

// ============================================================
// RWLock 实现
// ============================================================
RWLock::RWLock() = default;

void RWLock::read_lock() {
    mutex_.lock_shared();
}

void RWLock::read_unlock() {
    mutex_.unlock_shared();
}

void RWLock::write_lock() {
    mutex_.lock();
}

void RWLock::write_unlock() {
    mutex_.unlock();
}

bool RWLock::try_read_lock() {
    return mutex_.try_lock_shared();
}

bool RWLock::try_write_lock() {
    return mutex_.try_lock();
}

template<typename Rep, typename Period>
bool RWLock::try_read_lock_for(const std::chrono::duration<Rep, Period>& duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_read_lock()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

template<typename Rep, typename Period>
bool RWLock::try_write_lock_for(const std::chrono::duration<Rep, Period>& duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_write_lock()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// ============================================================
// RWLock2 实现
// ============================================================
RWLock2::RWLock2() : readers_(0), writers_waiting_(0), writer_active_(false) {}

void RWLock2::read_lock() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] {
        return !writer_active_ && writers_waiting_ == 0;
    });
    readers_++;
}

void RWLock2::read_unlock() {
    std::unique_lock<std::mutex> lock(mutex_);
    readers_--;
    if (readers_ == 0) {
        cv_.notify_all();
    }
}

void RWLock2::write_lock() {
    std::unique_lock<std::mutex> lock(mutex_);
    writers_waiting_++;
    cv_.wait(lock, [this] {
        return !writer_active_ && readers_ == 0;
    });
    writers_waiting_--;
    writer_active_ = true;
}

void RWLock2::write_unlock() {
    std::unique_lock<std::mutex> lock(mutex_);
    writer_active_ = false;
    cv_.notify_all();
}

bool RWLock2::try_read_lock() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!writer_active_ && writers_waiting_ == 0) {
        readers_++;
        return true;
    }
    return false;
}

bool RWLock2::try_write_lock() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!writer_active_ && readers_ == 0) {
        writers_waiting_++;
        writer_active_ = true;
        writers_waiting_--;
        return true;
    }
    return false;
}

int RWLock2::num_readers() const {
    return readers_;
}

bool RWLock2::has_writer_active() const {
    return writer_active_;
}

int RWLock2::num_writers_waiting() const {
    return writers_waiting_;
}

// ============================================================
// LockGuard 实现
// ============================================================
template<typename Lockable>
LockGuard<Lockable>::LockGuard(Lockable& lock) : lock_(lock), owned_(true) {
    lock_.lock();
}

template<typename Lockable>
LockGuard<Lockable>::~LockGuard() {
    if (owned_) {
        lock_.unlock();
    }
}

template<typename Lockable>
void LockGuard<Lockable>::unlock() {
    lock_.unlock();
    owned_ = false;
}

template<typename Lockable>
bool LockGuard<Lockable>::owns_lock() const {
    return owned_;
}

template<typename Lockable>
LockGuard<Lockable>::operator bool() const {
    return owned_;
}

// ============================================================
// TryLockGuard 实现
// ============================================================
template<typename Lockable>
TryLockGuard<Lockable>::TryLockGuard(Lockable& lock) : lock_(lock), owned_(lock.try_lock()) {}

template<typename Lockable>
TryLockGuard<Lockable>::~TryLockGuard() {
    if (owned_) {
        lock_.unlock();
    }
}

template<typename Lockable>
bool TryLockGuard<Lockable>::owns_lock() const {
    return owned_;
}

template<typename Lockable>
TryLockGuard<Lockable>::operator bool() const {
    return owned_;
}

template<typename Lockable>
void TryLockGuard<Lockable>::unlock() {
    if (owned_) {
        lock_.unlock();
        owned_ = false;
    }
}

// ============================================================
// ReadLockGuard 实现
// ============================================================
template<typename Lockable>
ReadLockGuard<Lockable>::ReadLockGuard(Lockable& lock) : lock_(lock), owned_(true) {
    lock_.read_lock();
}

template<typename Lockable>
ReadLockGuard<Lockable>::~ReadLockGuard() {
    if (owned_) {
        lock_.read_unlock();
    }
}

template<typename Lockable>
void ReadLockGuard<Lockable>::unlock() {
    lock_.read_unlock();
    owned_ = false;
}

template<typename Lockable>
bool ReadLockGuard<Lockable>::owns_lock() const {
    return owned_;
}

template<typename Lockable>
ReadLockGuard<Lockable>::operator bool() const {
    return owned_;
}

// ============================================================
// WriteLockGuard 实现
// ============================================================
template<typename Lockable>
WriteLockGuard<Lockable>::WriteLockGuard(Lockable& lock) : lock_(lock), owned_(true) {
    lock_.write_lock();
}

template<typename Lockable>
WriteLockGuard<Lockable>::~WriteLockGuard() {
    if (owned_) {
        lock_.write_unlock();
    }
}

template<typename Lockable>
void WriteLockGuard<Lockable>::unlock() {
    lock_.write_unlock();
    owned_ = false;
}

template<typename Lockable>
bool WriteLockGuard<Lockable>::owns_lock() const {
    return owned_;
}

template<typename Lockable>
WriteLockGuard<Lockable>::operator bool() const {
    return owned_;
}

// ============================================================
// Semaphore 实现
// ============================================================
Semaphore::Semaphore(int initial_count)
    : count_(initial_count), max_count_(initial_count) {}

void Semaphore::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return count_ > 0; });
    count_--;
}

bool Semaphore::try_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (count_ > 0) {
        count_--;
        return true;
    }
    return false;
}

template<typename Rep, typename Period>
bool Semaphore::wait_for(const std::chrono::duration<Rep, Period>& duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, duration, [this] { return count_ > 0; })) {
        return false;
    }
    count_--;
    return true;
}

void Semaphore::post() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ < max_count_) {
            count_++;
        }
    }
    cv_.notify_one();
}

void Semaphore::post_all() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count_ = max_count_;
    }
    cv_.notify_all();
}

int Semaphore::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

// ============================================================
// CountDownLatch 实现
// ============================================================
CountDownLatch::CountDownLatch(int count) : count_(count) {}

void CountDownLatch::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return count_ == 0; });
}

template<typename Rep, typename Period>
bool CountDownLatch::wait_for(const std::chrono::duration<Rep, Period>& duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, duration, [this] { return count_ == 0; });
}

void CountDownLatch::count_down() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ > 0) {
            count_--;
        }
    }
    if (count_ == 0) {
        cv_.notify_all();
    }
}

int CountDownLatch::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

// ============================================================
// CyclicBarrier 实现
// ============================================================
CyclicBarrier::CyclicBarrier(int parties)
    : parties_(parties), count_(parties), generation_(0) {}

int CyclicBarrier::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    int gen = generation_;
    int index = --count_;
    if (index == 0) {
        generation_++;
        count_ = parties_;
        cv_.notify_all();
    } else {
        cv_.wait(lock, [this, gen] { return gen != generation_; });
    }
    return index;
}

void CyclicBarrier::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_++;
    count_ = parties_;
    cv_.notify_all();
}

int CyclicBarrier::parties() const {
    return parties_;
}

int CyclicBarrier::waiting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parties_ - count_;
}

// ============================================================
// ShardedLock 实现
// ============================================================
template<typename LockType>
ShardedLock<LockType>::ShardedLock(size_t num_shards)
    : shards_(num_shards), num_shards_(num_shards) {}

template<typename LockType>
LockType& ShardedLock<LockType>::get_shard(const std::string& key) {
    size_t hash = std::hash<std::string>{}(key);
    return shards_[hash % num_shards_];
}

template<typename LockType>
LockType& ShardedLock<LockType>::get_shard(size_t hash) {
    return shards_[hash % num_shards_];
}

template<typename LockType>
size_t ShardedLock<LockType>::num_shards() const {
    return num_shards_;
}

template<typename LockType>
std::vector<LockType>& ShardedLock<LockType>::shards() {
    return shards_;
}

// 显式实例化常用类型
template class ShardedLock<SpinLock>;
template class ShardedLock<Mutex>;

// ============================================================
// ShardedRWLock 实现
// ============================================================
ShardedRWLock::ShardedRWLock(size_t num_shards)
    : shards_(num_shards), num_shards_(num_shards) {}

RWLock& ShardedRWLock::get_shard(const std::string& key) {
    size_t hash = std::hash<std::string>{}(key);
    return shards_[hash % num_shards_];
}

RWLock& ShardedRWLock::get_shard(size_t hash) {
    return shards_[hash % num_shards_];
}

size_t ShardedRWLock::num_shards() const {
    return num_shards_;
}

std::vector<RWLock>& ShardedRWLock::shards() {
    return shards_;
}

}  // namespace cc_server
