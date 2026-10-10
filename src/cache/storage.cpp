#include "storage.h"
#include "base/int_parse.h"
#include "base/log.h"
#include <mutex>
#include <cassert>
#include <limits>
#include <random>

namespace cc_server {

    GlobalStorage::GlobalStorage() {
        stores_.resize(kDefaultShards);
        mutexes_ = std::make_unique<std::shared_mutex[]>(kDefaultShards);

        // 防御性检查：确保初始化成功
        assert(stores_.size() == kDefaultShards && "GlobalStorage - stores_ init failed");

        LOG_INFO(STORAGE, "GlobalStorage initialized with %zu shards", kDefaultShards);
    }

    // 单例模式：Magic Static（C++11 线程安全局部静态变量）
    // 多个线程同时调用只会初始化一次
    GlobalStorage& GlobalStorage::instance() {
        static GlobalStorage instance;
        return instance;
    }

    // get - 获取键对应的值
    // @param key 键
    // @return 键对应的值，不存在返回空字符串
    // @note 线程安全：使用共享锁，允许多读并发

    std::optional<CacheObject> GlobalStorage::get(const std::string& key) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::get - key is empty");

        // 步骤1: 计算 key 应该去哪个分片
        const size_t shard_idx = get_shard_index(key);

        // 步骤2: 在对应分片加共享锁（多个读可以同时进行）
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 步骤3: 在对应分片的 unordered_map 中查找
        auto& store = stores_[shard_idx];

        auto it = store.find(key);
        if (it == store.end()) {
            LOG_DEBUG(STORAGE, "Get key=%s - not found", key.c_str());
            return std::nullopt;
        }

        // 惰性删除：过期只看 ExpireDict 这一份真相
        if (expire_dict_.is_expired(key)) {
            LOG_DEBUG(STORAGE, "Get key=%s - expired, triggering lazy delete", key.c_str());
            lock.unlock();
            del(key);
            return std::nullopt;
        }

        // 更新访问时间（原子写，避免共享锁下的数据竞争；修复 P1-4）
        // 读取 LRU 时间戳仅在 evict_one 的 pass2 独占锁下发生，此处写与读无并发竞争。
        it->second.last_access_time_ms.store(current_time_ms(), std::memory_order_relaxed);

        LOG_TRACE(STORAGE, "Get key=%s - found, shard=%zu", key.c_str(), shard_idx);
        return it->second.value;
    }

    // set - 设置键值对
    // @param key 键
    // @param value 值
    // @note 线程安全：使用独占锁，写操作互斥
    // @note 键已存在则更新值，不存在则插入新键值对

    void GlobalStorage::set(const std::string& key, const CacheObject& value) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::set - key is empty");

        // 检查是否需要淘汰（在获取锁之前检查，避免长时间持锁）
        evict_if_needed(key);

        // 1. 计算应该去哪个分片
        const size_t shard_idx  = get_shard_index(key);
        // 2. 在对应的分片加独占锁
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);  // 独占锁：写时阻塞所有读写

        // 如果键已经存在 清除过期时间
        bool had_expiration = expire_dict_.contains(key);
        expire_dict_.remove(key);

        // 获取当前时间
        int64_t now = current_time_ms();

        // 在对应分片的unordered_map里面插入或者更新
        CacheEntry entry(value, now);
        stores_[shard_idx].insert_or_assign(key, std::move(entry));

        // 增加脏计数器
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);

        if (had_expiration) {
            LOG_DEBUG(STORAGE, "Set key=%s - value updated, cleared expiration, shard=%zu",
                     key.c_str(), shard_idx);
        } else {
            LOG_DEBUG(STORAGE, "Set key=%s - new key inserted, shard=%zu",
                     key.c_str(), shard_idx);
        }
    }

    bool GlobalStorage::mutate(const std::string& key, ObjectType want_type,
                               const std::function<StoreOp(CacheObject&)>& fn,
                               StringPromotion promote) {
        assert(!key.empty() && "GlobalStorage::mutate - key is empty");

        // 与 set() 一致：在拿分片锁之前做淘汰检查。evict_if_needed 会去锁各分片
        // 找受害者，在独占锁里调它就是自己等自己。
        evict_if_needed(key);

        const size_t shard_idx = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        auto& store = stores_[shard_idx];
        auto it = store.find(key);

        // 过期的键当作不存在，并且顺手清掉
        if (it != store.end() && expire_dict_.is_expired(key)) {
            store.erase(it);
            expire_dict_.remove(key);
            it = store.end();
        }

        if (it != store.end()) {
            const ObjectType current_type = it->second.value.type();
            const bool promotable = current_type == ObjectType::STRING &&
                                    promote == StringPromotion::kAllow;
            if (current_type != want_type && !promotable) {
                return false;  // 调用方据此回 WRONGTYPE
            }
        }

        const bool existed = (it != store.end());
        // 键不存在时给一个默认构造对象（STRING 且为空），容器操作会把它接管成 want_type
        CacheObject obj = existed ? it->second.value : CacheObject();

        const StoreOp op = fn(obj);

        if (op == StoreOp::kNoop) {
            return true;
        }

        if (op == StoreOp::kErase) {
            if (existed) {
                store.erase(it);
                expire_dict_.remove(key);
                dirty_counter_.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }

        if (existed) {
            // 只替换 value：TTL 是键的属性，容器写入不应该把它清掉
            it->second.value = std::move(obj);
            it->second.last_access_time_ms.store(current_time_ms(), std::memory_order_relaxed);
        } else {
            stores_[shard_idx].insert_or_assign(key, CacheEntry(std::move(obj), current_time_ms()));
        }
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // del - 删除键值对
    // @param key 键
    // @return true 表示键存在且被删除，false 表示键不存在
    // @note 线程安全：使用独占锁

    bool GlobalStorage::del(const std::string& key) {
        // 参数校验
        assert(!key.empty() && "GlobalStorage::del - key is empty");

        const size_t shard_idx  = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 删除过期记录
        expire_dict_.remove(key);

        // 删除键值对
        size_t erased = stores_[shard_idx].erase(key);

        if (erased > 0) {
            // 增加脏计数器
            dirty_counter_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG(STORAGE, "Delete key=%s - success, shard=%zu", key.c_str(), shard_idx);
        }

        return erased > 0;
    }


    // exist - 检查键是否存在
    // @param key 键
    // @return true 键存在，false 键不存在
    // @note 线程安全：使用共享锁

    bool GlobalStorage::exist(const std::string& key) const {
        assert(!key.empty() && "GlobalStorage::exist - key is empty");

        const size_t shard_idx  = get_shard_index(key);
        std::shared_lock<std::shared_mutex> lock(mutexes_[shard_idx]);
        bool in_store = stores_[shard_idx].contains(key);
        if (!in_store) return false;
        // 一致性修复（P1-12）：已过期未删除的键不应被 EXIST/DBSIZE 计入
        if (expire_dict_.is_expired(key)) return false;
        return true;
    }


    // size - 获取存储的键值对数量
    // @return 当前存储的元素数量
    // @note 线程安全：使用共享锁，允许多读并发

    size_t GlobalStorage::size() const {
        size_t total = 0;
        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            total += stores_[i].size();
        }

        LOG_TRACE(STORAGE, "Size query - total=%zu", total);
        return total;
    }


    // clear - 清空所有键值对
    // @note 线程安全：遍历所有分片，每个分片加独占锁
    // @note 时间复杂度 O(num_shards + total_elements)
    //
    // 注意：这是一个代价较高的操作，需要锁定所有分片

    void GlobalStorage::clear() {
        size_t total_cleared = 0;

        // 遍历所有分片
        for (size_t i = 0; i < num_shards_; ++i) {
            // 对每个分片加独占锁
            std::unique_lock<std::shared_mutex> lock(mutexes_[i]);
            total_cleared += stores_[i].size();
            stores_[i].clear();
        }
        // 同时清除所有过期记录
        expire_dict_.clear_all();

        LOG_INFO(STORAGE, "Clear all - cleared %zu keys", total_cleared);
    }

    /*
     *
     * ARU 淘汰方法
     *
     */

    // 修复 P2-perf（淘汰性能悬崖）：近似 LRU 采用「随机分片采样」——
    // 旧 evict_one 是两遍全库扫描（64 分片全遍历找出全局最老 key，再回到
    // 该分片删除），在 200 万 key、需淘汰 80 万个的规模下是数十万次全库扫描，
    // 且多个写线程并发进入无互斥，写入延迟从微秒级飙升到分钟级。
    // 新实现：随机挑一个分片，加锁后在片内找最老的 key 并当场删除，
    // 单次代价 O(分片内条目数) ≈ 全库/64。
    std::string GlobalStorage::evict_one() {
        static thread_local std::mt19937 rng{std::random_device{}()};

        for (int attempt = 0; attempt < 8; ++attempt) {
            // 随机选一个分片（避免每次都扫 0 号分片造成倾斜）
            size_t shard = std::uniform_int_distribution<size_t>(
                0, num_shards_ - 1)(rng);

            {
                std::unique_lock<std::shared_mutex> lock(mutexes_[shard]);
                auto& store = stores_[shard];
                if (store.empty()) {
                    continue;  // 该分片空，换下一个
                }

                // 片内找最老的 key（已过期 key 优先删除，等于免费清理）
                auto oldest_it = store.end();
                int64_t oldest_time = std::numeric_limits<int64_t>::max();
                auto expired_it = store.end();

                const int64_t now = current_time_ms();
                for (auto it = store.begin(); it != store.end(); ++it) {
                    const int64_t expire_at = expire_dict_.get_expire_time(it->first);
                    if (expire_at > 0 && now >= expire_at) {
                        expired_it = it;
                        break;
                    }
                    if (it->second.last_access_time_ms < oldest_time) {
                        oldest_time = it->second.last_access_time_ms;
                        oldest_it = it;
                    }
                }

                auto victim_it = (expired_it != store.end()) ? expired_it : oldest_it;
                if (victim_it == store.end()) {
                    continue;
                }

                std::string victim_key = victim_it->first;
                store.erase(victim_it);
                expire_dict_.remove(victim_key);

                LOG_DEBUG(CACHE, "Evicted key=%s from shard=%zu (sampling LRU)",
                          victim_key.c_str(), shard);
                return victim_key;
            }
        }

        LOG_WARN(CACHE, "Evict_one - no key found after %d sampled shards", 8);
        return "";
    }

    void GlobalStorage::evict_if_needed(const std::string& hint_key) {
        (void)hint_key; // unused
        // 防御性检查：阈值必须有效
        size_t threshold = static_cast<size_t>(static_cast<double>(max_entries_) * EvictionConfig::kEvictThreshold);
        assert(threshold <= max_entries_ && "GlobalStorage::evict_if_needed - threshold exceeds max");

        size_t current_size = size();

        // 未超过阈值，不需要淘汰
        if (current_size < threshold) {
            return;
        }

        LOG_WARN(CACHE, "Cache full (size=%zu, max=%zu, threshold=%zu), starting eviction",
                current_size, max_entries_, threshold);

        // 淘汰到安全线（留出 40% 空间）
        size_t target_size = static_cast<size_t>(static_cast<double>(max_entries_) * EvictionConfig::kEvictTargetRatio);

        // 防御性检查：目标大小必须有效
        assert(target_size < max_entries_ && "GlobalStorage::evict_if_needed - target_size invalid");

        // 修复 P2-perf：限制单次淘汰循环的最多次数并中途复查 size()。
        // 旧循环只依赖本地计数，若与并发写入脱节可能一次淘汰远超必要数量。
        size_t evicted_count = 0;
        constexpr size_t kMaxEvictPerCycle = 1024;
        while (evicted_count < kMaxEvictPerCycle) {
            if (evict_one().empty()) {
                LOG_WARN(CACHE, "Eviction stopped - no more keys to evict, evicted=%zu", evicted_count);
                break;
            }
            ++evicted_count;
            if (evicted_count % 64 == 0) {
                if (size() <= target_size) {
                    break;
                }
            }
        }

        LOG_INFO(CACHE, "Eviction complete - evicted %zu keys, current_size=%zu, target_size=%zu",
                evicted_count, size(), target_size);
    }

    std::vector<std::pair<std::string, CacheObject> > GlobalStorage::get_all_objects() const {
        std::vector<std::pair<std::string, CacheObject>> result;
        result.reserve(size());

        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            for (const auto& [key, entry] : stores_[i]) {
                if (!expire_dict_.is_expired(key)) {
                    result.emplace_back(key, entry.value);
                }
            }
        }
        return result;
    }

    std::vector<KVWithTTL> GlobalStorage::get_all_objects_with_ttl() const {
        std::vector<KVWithTTL> result;
        result.reserve(size());

        // 获取当前时间
        int64_t now = current_time_ms();

        for (size_t i = 0; i < num_shards_; i++) {
            std::shared_lock<std::shared_mutex> lock(mutexes_[i]);
            for (const auto& [key, entry] : stores_[i]) {
                // 获取过期时间
                int64_t expire_time_ms = expire_dict_.get_expire_time(key);

                // get_expire_time 返回 -1 表示永不过期
                // 如果有过期时间且已过期，则跳过
                if (expire_time_ms > 0 && now >= expire_time_ms) {
                    continue;  // 已过期，跳过
                }

                result.emplace_back(key, entry.value, expire_time_ms);
            }
        }
        return result;
    }

    void GlobalStorage::set_expire(const std::string& key, int64_t ttl_ms) {
        if (key.empty() || ttl_ms <= 0) return;

        // 设置过期时间
        expire_dict_.set(key, ttl_ms);
        LOG_DEBUG(STORAGE, "Set expire for key=%s, ttl_ms=%ld", key.c_str(), ttl_ms);
    }

    bool GlobalStorage::set_conditional(const std::string& key, const CacheObject& value,
                                        SetCondition condition, int64_t ttl_ms, bool keep_ttl,
                                        std::optional<CacheObject>& previous) {
        assert(!key.empty() && "GlobalStorage::set_conditional - key is empty");
        previous.reset();

        // 和 set() 一样在取锁之前做淘汰检查，避免长时间持锁
        evict_if_needed(key);

        const size_t shard_idx = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        auto& store = stores_[shard_idx];
        auto it = store.find(key);
        const int64_t now = current_time_ms();

        // 已经到点的键按"不存在"算：Redis 的 lookupKeyWrite 就是这么处理的。
        // 留着它再写一遍会留下一条已经没人认的过期记录。
        bool present = (it != store.end());
        if (present && expire_dict_.is_expired(key)) {
            store.erase(it);
            expire_dict_.remove(key);
            present = false;
        }

        previous = present ? std::optional<CacheObject>(it->second.value) : std::nullopt;

        if (condition == SetCondition::kOnlyIfAbsent && present) return false;
        if (condition == SetCondition::kOnlyIfExists && !present) return false;

        // KEEPTTL 要保住的是"绝对过期时刻"，不是剩余时长：拿剩余时长再设一遍会
        // 因为这段时间里的执行耗时而漂移，而且中途键可能被别的写清掉了。
        const int64_t kept_expire_at = keep_ttl ? expire_dict_.get_expire_time(key) : -1;

        CacheEntry entry(value, now);
        store.insert_or_assign(key, std::move(entry));
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);

        if (keep_ttl) {
            if (kept_expire_at > 0) {
                expire_dict_.set_expire_time(key, kept_expire_at);
            } else {
                expire_dict_.remove(key);
            }
        } else if (ttl_ms > 0) {
            // 饱和加：和 set_with_expire() 同一个理由，极大的毫秒数会绕成负数，
            // "设了个很长的 TTL"变成"立刻就过期"。
            constexpr int64_t max_ms = std::numeric_limits<int64_t>::max();
            const int64_t expire_at = (ttl_ms > max_ms - now) ? max_ms : now + ttl_ms;
            expire_dict_.set_expire_time(key, expire_at);
        } else {
            expire_dict_.remove(key);  // 不带 TTL 的 SET 清掉旧 TTL，和 set() 一致
        }

        LOG_DEBUG(STORAGE, "Set_conditional key=%s applied condition=%d ttl_ms=%ld keep_ttl=%d shard=%zu",
                  key.c_str(), static_cast<int>(condition), static_cast<long>(ttl_ms),
                  keep_ttl ? 1 : 0, shard_idx);
        return true;
    }

    void GlobalStorage::set_with_expire(const std::string& key, const CacheObject& value, int64_t ttl_ms) {
        assert(!key.empty() && "GlobalStorage::set_with_expire - key is empty");

        // 检查是否需要淘汰
        evict_if_needed(key);

        // 计算应该去哪个分片
        const size_t shard_idx = get_shard_index(key);
        // 在对应的分片加独占锁
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 获取当前时间
        int64_t now = current_time_ms();

        // 设置值；过期时间只写 ExpireDict 这一份真相
        CacheEntry entry(value, now);
        if (ttl_ms > 0) {
            // 饱和加：SETEX 给一个极大的秒数时 now + ttl_ms 会绕成负数，
            // "设了个很长的 TTL"就变成"立刻就过期"，键被下一次读直接删掉。
            constexpr int64_t max_ms = std::numeric_limits<int64_t>::max();
            const int64_t expire_at = (ttl_ms > max_ms - now) ? max_ms : now + ttl_ms;
            expire_dict_.set_expire_time(key, expire_at);
            LOG_DEBUG(STORAGE, "Set_with_expire key=%s, ttl_ms=%ld, expire_at=%ld, shard=%zu",
                     key.c_str(), ttl_ms, expire_at, shard_idx);
        } else {
            expire_dict_.remove(key);
            LOG_DEBUG(STORAGE, "Set_with_expire key=%s (no expire), shard=%zu",
                     key.c_str(), shard_idx);
        }
        stores_[shard_idx].insert_or_assign(key, std::move(entry));

        // 增加脏计数器
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief 原子增加/减少键值（INCR/DECR/INCRBY/DECRBY 内部实现）
     *
     * 在 shard 独占锁内完成 读-改-写，保证并发 INCR 不丢更新。
     * 返回新值；若当前值非整数或递增会溢出 int64，返回 nullopt。
     */
    std::optional<int64_t> GlobalStorage::incrby(const std::string& key, int64_t delta) {
        assert(!key.empty() && "GlobalStorage::incrby - key is empty");

        // 淘汰检查（与 set 一致，避免长时间持锁前先做）
        evict_if_needed(key);

        const size_t shard_idx = get_shard_index(key);
        std::unique_lock<std::shared_mutex> lock(mutexes_[shard_idx]);

        // 惰性删除：若已过期，当作不存在（从 0 起算）
        if (expire_dict_.is_expired(key)) {
            stores_[shard_idx].erase(key);
            expire_dict_.remove(key);
        }

        int64_t val = 0;
        auto it = stores_[shard_idx].find(key);
        if (it != stores_[shard_idx].end()) {
            // 值必须是字符串类型且可解析为整数
            auto str_val = it->second.value.get_string();
            if (!str_val.has_value()) {
                return std::nullopt;  // 非字符串值（列表/哈希等）→ 调用方应回 WRONGTYPE
            }
            long long parsed = 0;
            if (!base::parse_ll_strict(str_val.value(), parsed)) {
                return std::nullopt;  // 非整数 → 调用方应回 "value is not an integer"
            }
            val = parsed;
        }

        // 溢出检查：delta > 0 时 val > INT64_MAX - delta 会溢出
        if ((delta > 0 && val > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && val < std::numeric_limits<int64_t>::min() - delta)) {
            return std::nullopt;  // 溢出 → 调用方应回 "increment or decrement would overflow"
        }

        val += delta;

        int64_t now = current_time_ms();
        stores_[shard_idx].insert_or_assign(key, CacheEntry(CacheObject(std::to_string(val)), now));
        dirty_counter_.fetch_add(1, std::memory_order_relaxed);

        return val;
    }

}
