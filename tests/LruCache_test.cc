#include "LRU/LruCache.h"

#include <gtest/gtest.h>

#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Policy.h"

namespace {

using louis::cache::LeaveReason;
using louis::cache::LruCache;

// 基本的 put / get 功能
TEST(LruCacheTest, PutAndGetValue) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
}

// 未命中时返回 false，且不修改传出参数
TEST(LruCacheTest, GetMissReturnsFalseAndKeepsValue) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);

    int value = -1;
    EXPECT_FALSE(cache.get(99, value));
    EXPECT_EQ(value, -1);
}

// 值语义的 get 未命中时返回 Value 的默认值
TEST(LruCacheTest, GetByValueReturnsDefaultOnMiss) {
    LruCache<int, int> cache(2);
    EXPECT_EQ(cache.get(42), 0);
}

// put 已存在的 key 会更新值，且不增加条目数
TEST(LruCacheTest, PutUpdatesExistingValue) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(1, 99);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 99);
    EXPECT_EQ(cache.size(), 1u);
}

// 容量满时淘汰最久未使用的条目
TEST(LruCacheTest, EvictsLeastRecentlyUsedWhenFull) {
    LruCache<int, int> cache(3);
    for (int i = 1; i <= 3; ++i) {
        cache.put(i, i * 10);
    }

    cache.put(4, 40);  // 淘汰最久未使用的 1

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
    EXPECT_EQ(cache.size(), 3u);
    for (int i = 2; i <= 4; ++i) {
        ASSERT_TRUE(cache.get(i, value));
        EXPECT_EQ(value, i * 10);
    }
}

// get 会刷新条目的新旧程度，改变后续淘汰顺序
TEST(LruCacheTest, GetRefreshesRecency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));  // 1 变为最近使用，2 成为最久未使用

    cache.put(3, 30);  // 淘汰 2 而不是 1
    EXPECT_FALSE(cache.get(2, value));
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
}

// 更新已存在的 key 同样会刷新其新旧程度
TEST(LruCacheTest, PutExistingKeyRefreshesRecency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.put(1, 11);  // 更新 1 并刷新
    cache.put(3, 30);  // 淘汰 2

    int value = 0;
    EXPECT_FALSE(cache.get(2, value));
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
}

// 淘汰顺序严格跟随访问（put/get）的新旧程度
TEST(LruCacheTest, EvictionOrderFollowsAccessRecency) {
    LruCache<int, int> cache(4);
    for (int i = 1; i <= 4; ++i) {
        cache.put(i, i * 10);  // 新旧顺序（最近→最旧）：4,3,2,1
    }

    int value = 0;
    cache.get(1, value);  // 1,4,3,2
    cache.get(2, value);  // 2,1,4,3

    cache.put(5, 50);  // 淘汰 3
    cache.put(6, 60);  // 淘汰 4

    EXPECT_FALSE(cache.get(3, value));
    EXPECT_FALSE(cache.get(4, value));
    EXPECT_TRUE(cache.get(1, value));
    EXPECT_TRUE(cache.get(2, value));
    EXPECT_TRUE(cache.get(5, value));
    EXPECT_TRUE(cache.get(6, value));
    EXPECT_EQ(cache.size(), 4u);
}

// 连续插入大量条目后，只保留最近 capacity 个
TEST(LruCacheTest, SlidingWindowEviction) {
    LruCache<int, int> cache(3);
    for (int i = 0; i < 100; ++i) {
        cache.put(i, i);
    }

    EXPECT_EQ(cache.size(), 3u);
    int value = 0;
    for (int i = 0; i < 97; ++i) {
        EXPECT_FALSE(cache.get(i, value));
    }
    for (int i = 97; i < 100; ++i) {
        EXPECT_TRUE(cache.get(i, value));
    }
}

// 容量恰好等于条目数时不淘汰
TEST(LruCacheTest, HoldsExactlyCapacityItems) {
    LruCache<int, int> cache(5);
    for (int i = 0; i < 5; ++i) {
        cache.put(i, i);
    }
    EXPECT_EQ(cache.size(), 5u);

    cache.put(5, 5);
    EXPECT_EQ(cache.size(), 5u);
}

// 容量 0：put 直接丢弃，size 恒为 0，get 恒未命中
TEST(LruCacheTest, ZeroCapacityDropsAllPuts) {
    LruCache<int, int> cache(0);
    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_EQ(cache.size(), 0u);
    int value = -1;
    EXPECT_FALSE(cache.get(1, value));
    EXPECT_EQ(value, -1);
    EXPECT_EQ(cache.get(2), 0);
}

// remove 删除指定条目
TEST(LruCacheTest, RemoveErasesEntry) {
    LruCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.remove(1);

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
    EXPECT_EQ(cache.size(), 1u);
}

// remove 不存在的 key 不应崩溃，也不影响其他条目
TEST(LruCacheTest, RemoveNonexistentKeyIsNoop) {
    LruCache<int, int> cache(3);
    cache.put(1, 10);

    cache.remove(99);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    EXPECT_EQ(cache.size(), 1u);
}

// remove 后空出的容量不会被新插入立即占用为淘汰
TEST(LruCacheTest, RemoveFreesCapacity) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.remove(1);

    cache.put(3, 30);  // 尚未满，不应淘汰 2

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
    ASSERT_TRUE(cache.get(2, value));
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(cache.size(), 2u);
}

// remove 后可以重新插入同名的 key
TEST(LruCacheTest, PutAfterRemoveReinserts) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.remove(1);
    cache.put(1, 11);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    EXPECT_EQ(cache.size(), 1u);
}

// 非整型 Key/Value 类型
TEST(LruCacheTest, WorksWithStringKeyAndValue) {
    LruCache<std::string, std::string> cache(2);
    cache.put("a", "alpha");
    cache.put("b", "beta");

    cache.put("c", "gamma");  // 淘汰 "a"

    std::string value;
    EXPECT_FALSE(cache.get("a", value));
    ASSERT_TRUE(cache.get("b", value));
    EXPECT_EQ(value, "beta");
    ASSERT_TRUE(cache.get("c", value));
    EXPECT_EQ(value, "gamma");
}

// 多线程并发 put/get，验证线程安全且容量不被突破
TEST(LruCacheTest, ConcurrentPutAndGet) {
    constexpr int kCapacity = 64;
    constexpr int kThreads = 8;
    constexpr int kKeys = 1000;
    constexpr int kOpsPerThread = 2000;

    LruCache<int, int> cache(kCapacity);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
            for (int i = 0; i < kOpsPerThread; ++i) {
                int key = static_cast<int>(rng() % kKeys);
                if (rng() % 2 == 0) {
                    cache.put(key, key * 10);
                } else {
                    int value = 0;
                    // 命中时值必须与写入的一致
                    if (cache.get(key, value)) {
                        EXPECT_EQ(value, key * 10);
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_LE(cache.size(), static_cast<size_t>(kCapacity));
}

// ============ LeaveCallback 回调测试 ============

// 记录一次回调的 key / value / reason
struct LeaveRecord {
    int key;
    int value;
    LeaveReason reason;
};

// 安装回调并返回记录列表（列表由回调写入，测试结束时回收）
// 注意：回调在持锁状态下可能被并发触发，容器自带锁保护
class LeaveRecorder {
   public:
    explicit LeaveRecorder(LruCache<int, int>& cache) {
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

// 容量淘汰触发回调，reason 为 Evicted，且携带正确的 key/value
TEST(LruCacheLeaveCallbackTest, EvictionNotifiesWithEvictedReason) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // 容量满，淘汰 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// 显式 remove 触发回调，reason 为 Explicit
TEST(LruCacheLeaveCallbackTest, RemoveNotifiesWithExplicitReason) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.remove(1);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// remove 不存在的 key 不触发回调
TEST(LruCacheLeaveCallbackTest, RemoveNonexistentKeyDoesNotNotify) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.remove(99);

    EXPECT_TRUE(recorder.take().empty());
}

// 淘汰顺序遵循 LRU 语义：get 刷新过的条目不会被淘汰
TEST(LruCacheLeaveCallbackTest, EvictionFollowsRecencyOrder) {
    LruCache<int, int> cache(3);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);

    int value = 0;
    cache.get(1, value);  // 1 被刷新，最旧变为 2

    cache.put(4, 40);  // 淘汰 2

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 2);
    EXPECT_EQ(records[0].value, 20);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// 更新已存在的 key 不触发回调（条目没有离开缓存）
TEST(LruCacheLeaveCallbackTest, UpdatingExistingKeyDoesNotNotify) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(1, 99);  // 原地更新

    EXPECT_TRUE(recorder.take().empty());
}

// 普通读写（未淘汰）不触发回调
TEST(LruCacheLeaveCallbackTest, NormalGetDoesNotNotify) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    int value = 0;
    cache.get(1, value);

    EXPECT_TRUE(recorder.take().empty());
}

// 连续插入形成淘汰序列，回调按淘汰顺序逐条触发
TEST(LruCacheLeaveCallbackTest, SlidingWindowNotifiesInEvictionOrder) {
    LruCache<int, int> cache(3);
    LeaveRecorder recorder(cache);

    for (int i = 0; i < 8; ++i) {
        cache.put(i, i * 10);
    }

    auto records = recorder.take();
    // 前 3 条占满容量，后续 5 次插入各淘汰 1 条
    ASSERT_EQ(records.size(), 5u);
    for (size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].key, static_cast<int>(i));
        EXPECT_EQ(records[i].value, static_cast<int>(i) * 10);
        EXPECT_EQ(records[i].reason, LeaveReason::Evicted);
    }
}

// 混合 Explicit / Evicted 时 reason 区分正确
TEST(LruCacheLeaveCallbackTest, MixedRemoveAndEvictionReportCorrectReasons) {
    LruCache<int, int> cache(2);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.remove(1);   // Explicit
    cache.put(3, 30);  // 容量未满（remove 腾出了位置），不淘汰
    cache.put(4, 40);  // 容量满，淘汰 2，Evicted

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
    EXPECT_EQ(records[1].key, 2);
    EXPECT_EQ(records[1].value, 20);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// remove 后重新插入再淘汰，两条回调都携带当时的值
TEST(LruCacheLeaveCallbackTest, ReinsertedKeyNotifiesEachTime) {
    LruCache<int, int> cache(1);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.remove(1);
    cache.put(1, 11);
    cache.put(2, 20);  // 淘汰重新插入的 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
    EXPECT_EQ(records[1].value, 11);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// 字符串键值也能通过回调传出
TEST(LruCacheLeaveCallbackTest, WorksWithStringKeyAndValue) {
    LruCache<std::string, std::string> cache(1);

    std::vector<std::pair<std::string, std::string>> records;
    cache.setLeaveCallback([&](const std::string& k, const std::string& v, LeaveReason r) {
        ASSERT_EQ(r, LeaveReason::Evicted);
        records.emplace_back(k, v);
    });

    cache.put("a", "alpha");
    cache.put("b", "beta");  // 淘汰 "a"

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].first, "a");
    EXPECT_EQ(records[0].second, "alpha");
}

// remove 路径在锁外通知，回调内可安全访问缓存自身
TEST(LruCacheLeaveCallbackTest, RemoveCallbackCanCallCacheInsideCallback) {
    LruCache<int, int> cache(2);

    cache.put(1, 10);
    cache.put(2, 20);

    size_t sizeInsideCallback = 0;
    cache.setLeaveCallback([&](const int& /*k*/, const int& /*v*/, LeaveReason /*r*/) {
        // remove 先完成移除、释放锁，再通知；此时条目已不在缓存中
        sizeInsideCallback = cache.size();
    });

    cache.remove(1);

    EXPECT_EQ(sizeInsideCallback, 1u);
}

// evict 路径同样在锁外、且在条目移除完成后通知，回调内可安全访问缓存自身
TEST(LruCacheLeaveCallbackTest, EvictCallbackCanCallCacheInsideCallback) {
    LruCache<int, int> cache(2);

    cache.put(1, 10);
    cache.put(2, 20);

    size_t sizeInsideCallback = 0;
    cache.setLeaveCallback([&](const int& /*k*/, const int& /*v*/, LeaveReason r) {
        EXPECT_EQ(r, LeaveReason::Evicted);
        // 淘汰与新插入完成后才通知，缓存应持有 2 条（新条目已入、旧条目已走）
        sizeInsideCallback = cache.size();
    });

    cache.put(3, 30);  // 触发 evict 路径

    EXPECT_EQ(sizeInsideCallback, 2u);
}

// 容量 0：put 丢弃的条目未曾驻留，不触发回调
TEST(LruCacheLeaveCallbackTest, ZeroCapacityPutDoesNotNotify) {
    LruCache<int, int> cache(0);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(1, 11);

    EXPECT_TRUE(recorder.take().empty());
}

// ============ exists / capacity / peek ============

// exists 命中与未命中
TEST(LruCacheTest, ExistsReportsPresence) {
    LruCache<int, int> cache(2);
    EXPECT_FALSE(cache.exists(1));

    cache.put(1, 10);
    EXPECT_TRUE(cache.exists(1));
    EXPECT_FALSE(cache.exists(2));
}

// exists 是 peek 语义：不刷新 recency，不改变淘汰顺序
TEST(LruCacheTest, ExistsDoesNotRefreshRecency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_TRUE(cache.exists(1));  // 若误刷新 recency，1 会取代 2 成为幸存者

    cache.put(3, 30);  // 淘汰最久未使用的 1
    EXPECT_FALSE(cache.exists(1));
    EXPECT_TRUE(cache.exists(2));
    EXPECT_TRUE(cache.exists(3));
}

// peek 返回条目值，但同样不刷新 recency
TEST(LruCacheTest, PeekReturnsValueWithoutRefreshingRecency) {
    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);

    auto value = cache.peek(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 10);
    EXPECT_FALSE(cache.peek(99).has_value());

    cache.put(3, 30);  // 淘汰 1（peek 未刷新 recency）
    EXPECT_FALSE(cache.peek(1).has_value());
    EXPECT_TRUE(cache.peek(2).has_value());
}

// capacity 返回构造时设定的容量，且不随 put 改变
TEST(LruCacheTest, CapacityReturnsConfiguredValue) {
    LruCache<int, int> cache(5);
    EXPECT_EQ(cache.capacity(), 5u);

    cache.put(1, 10);
    EXPECT_EQ(cache.capacity(), 5u);
}

// 容量 0：条目不驻留，exists 恒为 false，capacity 为 0
TEST(LruCacheTest, ZeroCapacityExistsAlwaysFalse) {
    LruCache<int, int> cache(0);
    cache.put(1, 10);  // 被丢弃

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.exists(1));
    EXPECT_EQ(cache.capacity(), 0u);
}

}  // namespace
