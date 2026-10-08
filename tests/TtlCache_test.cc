#include "Ttl/TtlCache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "LRU/LruCache.h"
#include "Policy.h"

namespace {

using louis::cache::LeaveReason;
using louis::cache::LruCache;
using louis::cache::Policy;
using louis::cache::TtlCache;

// ============ ManualClock：可手动拨动的测试时钟 ============

// 满足最小 Clock 要求：duration / time_point / now()
// advance() 手动推进虚拟时间，测试零 sleep；仅限单线程测试使用（无内部同步）
class ManualClock {
   public:
    using rep = std::chrono::milliseconds::rep;
    using period = std::chrono::milliseconds::period;
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<ManualClock, duration>;

    static time_point now() { return current(); }

    // 推进虚拟时钟
    static void advance(duration d) { current() += d; }

    // 每个测试开始前归零，保证用例互不干扰
    static void reset() { current() = time_point{duration{0}}; }

   private:
    static time_point& current() {
        static time_point t{duration{0}};
        return t;
    }
};

using ManualTtlCache = TtlCache<int, int, ManualClock>;

// 便捷构造：LRU inner + 指定 TTL
std::shared_ptr<ManualTtlCache> makeTtl(
    std::shared_ptr<LruCache<int, int>> inner, std::chrono::milliseconds ttl
) {
    return std::make_shared<ManualTtlCache>(std::move(inner), ttl);
}

// ============ LeaveCallback 回调测试夹具 ============

// 记录一次回调的 key / value / reason
struct LeaveRecord {
    int key;
    int value;
    LeaveReason reason;
};

// 安装回调并返回记录列表（列表由回调写入，测试结束时回收）
// 回调可能被并发触发，容器自带锁保护
class LeaveRecorder {
   public:
    explicit LeaveRecorder(ManualTtlCache& cache) {
        cache.setLeaveCallback([this](const int& k, const int& v, LeaveReason r) {
            std::lock_guard<std::mutex> lock(mutex_);
            records_.push_back({k, v, r});
        });
    }

    std::vector<LeaveRecord> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(records_, {});
    }

   private:
    std::mutex mutex_;
    std::vector<LeaveRecord> records_;
};

// ============ 基础读写语义 ============

// TTL 内 get / exists 正常命中
TEST(TtlCacheTest, PutGetHitWithinTtl) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(50));

    int value = 0;
    ASSERT_TRUE(cache->get(1, value));
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(cache->exists(1));
    EXPECT_EQ(cache->get(1), 10);
}

// 过期后 get 返回 miss（两个重载），且不修改传出参数
TEST(TtlCacheTest, GetReturnsMissAfterExpiry) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(100));

    int value = -1;
    EXPECT_FALSE(cache->get(1, value));
    EXPECT_EQ(value, -1);
    EXPECT_EQ(cache->get(1), 0);  // Value 重载返回默认值
}

// 过期后 exists 返回 false（peek 语义的硬约束）
TEST(TtlCacheTest, ExistsReturnsFalseAfterExpiry) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));

    cache->put(1, 10);
    EXPECT_TRUE(cache->exists(1));
    ManualClock::advance(std::chrono::milliseconds(100));
    EXPECT_FALSE(cache->exists(1));
}

// 固定过期：get 命中不续期
TEST(TtlCacheTest, GetDoesNotRenewExpiry) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));

    cache->put(1, 10);  // expiry = 100
    ManualClock::advance(std::chrono::milliseconds(60));
    int value = 0;
    ASSERT_TRUE(cache->get(1, value));                    // t=60 命中；若续期，expiry 会变成 160
    ManualClock::advance(std::chrono::milliseconds(60));  // t=120
    EXPECT_FALSE(cache->get(1, value));                   // 固定过期：120 >= 100 已过期
}

// 重新 put 会重置过期时间
TEST(TtlCacheTest, PutRefreshesExpiry) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));

    cache->put(1, 10);  // expiry = 100
    ManualClock::advance(std::chrono::milliseconds(80));
    cache->put(1, 11);  // 重新 put → expiry = 180

    ManualClock::advance(std::chrono::milliseconds(90));  // t=170：未重置则已过期
    int value = 0;
    ASSERT_TRUE(cache->get(1, value));
    EXPECT_EQ(value, 11);

    ManualClock::advance(std::chrono::milliseconds(20));  // t=190 >= 180
    EXPECT_FALSE(cache->get(1, value));
}

// ============ 过期清除与回调 ============

// get 触发惰性过期，对外报 Expired 且携带正确 key/value
TEST(TtlCacheLeaveCallbackTest, LazyExpiryNotifiesExpiredWithKeyValue) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(100));

    int value = 0;
    EXPECT_FALSE(cache->get(1, value));  // get 触发惰性清除

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Expired);
}

// exists 同样触发惰性过期并报 Expired
TEST(TtlCacheLeaveCallbackTest, ExistsAlsoPurgesAndNotifiesExpired) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(100));

    EXPECT_FALSE(cache->exists(1));  // exists 触发惰性清除

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Expired);
}

// purgeExpired 只清除过期项并返回清除数量
TEST(TtlCacheTest, PurgeExpiredRemovesOnlyExpiredAndReturnsCount) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(8), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    cache->put(2, 20);
    cache->put(3, 30);
    ManualClock::advance(std::chrono::milliseconds(100));  // 1/2/3 全部过期
    cache->put(4, 40);                                     // 未过期，expiry = 200

    EXPECT_EQ(cache->purgeExpired(), 3u);  // 只清过期项
    EXPECT_EQ(cache->size(), 1u);
    EXPECT_TRUE(cache->exists(4));

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 3u);
    for (const auto& r : records) {
        EXPECT_EQ(r.reason, LeaveReason::Expired);
    }
}

// size() 先清除过期项再统计，过期项同时触发回调
TEST(TtlCacheTest, SizePurgesExpiredFirst) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(8), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    cache->put(2, 20);
    ManualClock::advance(std::chrono::milliseconds(100));  // 全部过期

    EXPECT_EQ(cache->size(), 0u);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    for (const auto& r : records) {
        EXPECT_EQ(r.reason, LeaveReason::Expired);
    }
}

// 用户显式 remove（未过期）转发 Explicit
TEST(TtlCacheLeaveCallbackTest, RemoveForwardsExplicit) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(50));
    cache->remove(1);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);

    int value = 0;
    EXPECT_FALSE(cache->get(1, value));
}

// inner 容量淘汰原样转发 Evicted（条目未过期）
TEST(TtlCacheLeaveCallbackTest, InnerEvictionForwardedAsEvicted) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(1), std::chrono::milliseconds(1000));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    cache->put(2, 20);  // inner 容量 1，挤出 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_TRUE(cache->exists(2));
}

// 僵尸条目（已过期未触碰）被 inner 容量挤出时，对外报 Expired 而非 Evicted
TEST(TtlCacheLeaveCallbackTest, ZombieEvictionConvertedToExpired) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(2), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    cache->put(2, 20);
    ManualClock::advance(std::chrono::milliseconds(100));  // 1/2 均过期但未触碰
    cache->put(3, 30);                                     // inner 挤出 LRU 条目 1 → zombie 转换

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Expired);
}

// inner 容量为 0：put 静默丢弃，无回调，expiryMap_ 不留残留
TEST(TtlCacheLeaveCallbackTest, NoCallbackForCapacityZeroInner) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(0), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);

    EXPECT_FALSE(cache->exists(1));
    EXPECT_EQ(cache->size(), 0u);
    ManualClock::advance(std::chrono::milliseconds(1000));
    EXPECT_EQ(cache->size(), 0u);  // 无残留可清
    EXPECT_TRUE(recorder.take().empty());
}

// 多线程并发 put/get（真实 steady_clock + 短 TTL），验证线程安全
TEST(TtlCacheTest, ConcurrentSmokeWithSteadyClock) {
    constexpr int kCapacity = 64;
    constexpr int kThreads = 5;
    constexpr int kOpsPerThread = 2000;

    auto cache = std::make_shared<TtlCache<int, int>>(
        std::make_shared<LruCache<int, int>>(kCapacity), std::chrono::milliseconds(50)
    );

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([cache, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
            for (int i = 0; i < kOpsPerThread; ++i) {
                int key = static_cast<int>(rng() % 32);
                if (rng() % 2 == 0) {
                    cache->put(key, key * 10);
                } else {
                    int value = 0;
                    // 命中时值必须与写入的一致
                    if (cache->get(key, value)) {
                        EXPECT_EQ(value, key * 10);
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_LE(cache->size(), static_cast<size_t>(kCapacity));
    cache->purgeExpired();
    EXPECT_LE(cache->size(), static_cast<size_t>(kCapacity));
}

// 复现 purgeExpired() 的非原子测量竞态。
//
// 不变式：TTL=1h，测试期间没有任何条目会过期，故每次 purgeExpired() 必须返回 0。
// 旧实现用「purge 前后两次 size() 做差」跨两个加锁窗口采样，写/删线程落在两窗口
// 缝隙里就会污染差值：
//   - 缝隙中 put  ⇒ clearSize > rawSize ⇒ size_t 下溢返回天文数字
//   - 缝隙中 remove/驱逐 ⇒ clearSize < rawSize ⇒ 无中生有的正数
// 注意：概率性复现，修复前偶发通过属正常，可多跑几轮；修复后应稳定通过。
TEST(TtlCacheTest, PurgeExpiredCountIsZeroWhenNothingExpired) {
    constexpr int kKeySpace = 512;
    auto cache = std::make_shared<TtlCache<int, int>>(
        std::make_shared<LruCache<int, int>>(1024), std::chrono::hours(1)
    );

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    // 2 写 2 删：让 inner 的 size 在随机游走中持续变化，扩大两窗口采样不一致的机会
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([cache, t, &stop] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
            while (!stop.load(std::memory_order_relaxed)) {
                const int key = static_cast<int>(rng() % kKeySpace);
                if (t < 2) {
                    cache->put(key, key);
                } else {
                    cache->remove(key);
                }
            }
        });
    }

    bool violated = false;
    size_t badValue = 0;
    for (int i = 0; i < 3000 && !violated; ++i) {
        badValue = cache->purgeExpired();
        violated = badValue != 0;
    }

    stop.store(true);
    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_FALSE(violated) << "无过期条目时 purgeExpired() 返回 " << badValue
                           << "（期望恒为 0）——非原子测量竞态";
}

// 已过期但未触碰的条目，用户显式 remove 仍报 Explicit（固化分类决策）
TEST(TtlCacheLeaveCallbackTest, RemoveExpiredKeyReportsExplicit) {
    ManualClock::reset();
    auto cache = makeTtl(std::make_shared<LruCache<int, int>>(4), std::chrono::milliseconds(100));
    LeaveRecorder recorder(*cache);

    cache->put(1, 10);
    ManualClock::advance(std::chrono::milliseconds(100));  // 已过期但未触碰
    cache->remove(1);                                      // 用户显式删除优先

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// 构造时 inner 为 null 抛 invalid_argument
TEST(TtlCacheTest, NullInnerThrowsInvalidArgument) {
    std::shared_ptr<Policy<int, int>> nullInner;
    EXPECT_THROW((ManualTtlCache(nullInner, std::chrono::milliseconds(100))), std::invalid_argument);
}

}  // namespace
