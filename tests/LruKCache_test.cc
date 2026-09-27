#include "LRU/LruKCache.h"

#include <gtest/gtest.h>

#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "Policy.h"

namespace {

using louis::cache::LeaveReason;
using louis::cache::LruKCache;

// ============ 基本访问与晋升语义（默认 k=2）============

// 首次 put 只进入历史队列：size 不变，get 视为未命中
TEST(LruKCacheTest, FirstPutGoesToHistory) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);

    EXPECT_EQ(cache.size(), 0u);
    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
}

// 第二次 put 晋升到主缓存，且携带本次 put 的新值而非历史条目里的旧值
TEST(LruKCacheTest, SecondPutPromotesWithLatestValue) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);
    cache.put(1, 11);  // 达到 k 次，晋升；值应为本次的 11

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.get(1), 11);
}

// get 同样累计访问次数：第二次 get 触发晋升，但本次仍视为未命中
TEST(LruKCacheTest, GetAccumulatesCountAndPromotes) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);  // 历史队列，count=1

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));  // count=2，晋升，但本次仍是未命中
    EXPECT_EQ(cache.size(), 1u);

    ASSERT_TRUE(cache.get(1, value));  // 后续访问命中主缓存
    EXPECT_EQ(value, 10);
}

// 历史命中不修改传出参数（即使本次访问触发了晋升）
TEST(LruKCacheTest, HistoryHitDoesNotTouchOutParam) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);

    int value = 42;
    EXPECT_FALSE(cache.get(1, value));
    EXPECT_EQ(value, 42);
}

// 完全未知的 key 未命中，且不影响传出参数
TEST(LruKCacheTest, UnknownKeyMisses) {
    LruKCache<int, int> cache(2, 4);

    int value = -1;
    EXPECT_FALSE(cache.get(99, value));
    EXPECT_EQ(value, -1);
    EXPECT_EQ(cache.size(), 0u);
}

// 值语义 get：主缓存命中返回值；仅历史命中或未知 key 返回默认值
TEST(LruKCacheTest, ValueGetOverload) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);
    EXPECT_EQ(cache.get(1), 0);  // 还在历史队列中

    cache.put(1, 11);  // 晋升
    EXPECT_EQ(cache.get(1), 11);
    EXPECT_EQ(cache.get(42), 0);
}

// k=3：需要三次访问（put/get 混合）才晋升，get 的计数写回历史
TEST(LruKCacheTest, KThreeRequiresThreeAccesses) {
    LruKCache<int, int> cache(2, 4, 3);
    cache.put(1, 10);  // count=1

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));  // count=2，未晋升
    EXPECT_EQ(cache.size(), 0u);

    EXPECT_FALSE(cache.get(1, value));  // count=3，晋升
    EXPECT_EQ(cache.size(), 1u);
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
}

// ============ remove 语义 ============

// remove 清空历史计数：remove 后需重新累计 k 次才晋升
TEST(LruKCacheTest, RemoveResetsHistoryCount) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);  // 历史 count=1
    cache.remove(1);

    cache.put(1, 20);  // 若历史未清理，这里会立即晋升
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);

    cache.put(1, 21);  // 第二次才晋升
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.get(1), 21);
}

// remove 主缓存条目后，key 重新从历史流程走起
TEST(LruKCacheTest, RemoveFromMainThenReinsert) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);
    cache.put(1, 11);  // 晋升
    EXPECT_EQ(cache.size(), 1u);

    cache.remove(1);
    EXPECT_EQ(cache.size(), 0u);

    cache.put(1, 12);  // 重新进入历史队列
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);
}

// remove 不存在的 key 是 no-op，不影响后续行为
TEST(LruKCacheTest, RemoveNonexistentKeyIsNoop) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);

    cache.remove(99);

    cache.put(1, 11);  // 正常累计到 2 次晋升
    EXPECT_EQ(cache.get(1), 11);
}

// ============ 主缓存容量与淘汰 ============

// 晋升时主缓存已满，会淘汰主缓存中的 LRU 条目，且被淘汰条目彻底离开
TEST(LruKCacheTest, PromotionEvictsLruFromMain) {
    LruKCache<int, int> cache(1, 4);
    cache.put(1, 10);
    cache.put(1, 11);  // 1 晋升，主缓存 {1:11}

    cache.put(2, 20);
    cache.put(2, 21);  // 2 晋升，挤掉 1

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.get(1), 0);  // 1 既不在主缓存也不在历史
    EXPECT_EQ(cache.get(2), 21);
}

// 主缓存淘汰遵循 LRU：get 刷新过的条目存活
TEST(LruKCacheTest, MainEvictionFollowsRecency) {
    LruKCache<int, int> cache(2, 8);
    cache.put(1, 10);
    cache.put(1, 11);  // main {1}
    cache.put(2, 20);
    cache.put(2, 21);  // main {1, 2}

    int value = 0;
    cache.get(1, value);  // 刷新 1，2 变为最旧

    cache.put(3, 30);
    cache.put(3, 31);  // 3 晋升，淘汰 2

    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    EXPECT_EQ(cache.get(2), 0);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 31);
}

// 更新主缓存已有 key：原地更新并刷新新旧程度
TEST(LruKCacheTest, UpdateExistingMainKeyRefreshesRecency) {
    LruKCache<int, int> cache(2, 8);
    cache.put(1, 10);
    cache.put(1, 11);
    cache.put(2, 20);
    cache.put(2, 21);  // main {1, 2}

    cache.put(1, 12);  // 更新 1 并刷新

    cache.put(3, 30);
    cache.put(3, 31);  // 3 晋升，淘汰 2 而非 1

    EXPECT_EQ(cache.get(2), 0);
    EXPECT_EQ(cache.get(1), 12);
    EXPECT_EQ(cache.get(3), 31);
}

// size 只统计主缓存，历史队列中的条目不计入
TEST(LruKCacheTest, SizeCountsOnlyMainCache) {
    LruKCache<int, int> cache(2, 8);
    cache.put(1, 10);
    cache.put(1, 11);  // main {1}

    for (int i = 2; i <= 4; ++i) {
        cache.put(i, i * 10);  // 只进历史队列
    }

    EXPECT_EQ(cache.size(), 1u);
}

// 历史队列容量：超出后按 LRU 淘汰历史条目，访问计数随之丢失
TEST(LruKCacheTest, HistoryCapacityEvictsOldestHistoryEntry) {
    LruKCache<int, int> cache(2, 1);  // 历史容量 1
    cache.put(1, 10);                 // history {1}
    cache.put(2, 20);                 // history 满，淘汰 1

    cache.put(1, 11);  // 1 的计数已丢失，重新从 1 开始
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);

    cache.put(1, 12);  // 第二次才晋升
    EXPECT_EQ(cache.get(1), 12);
}

// 历史容量 0：访问历史无处存放，k>=2 时永远无法晋升
TEST(LruKCacheTest, HistoryCapacityZeroNeverPromotes) {
    LruKCache<int, int> cache(2, 0);
    cache.put(1, 10);
    cache.put(1, 11);
    cache.put(1, 12);

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);
}

// ============ 并发 ============

// 多线程并发 put/get/remove：外层锁保证复合操作原子，命中值一致且容量不被突破
TEST(LruKCacheTest, ConcurrentPutGetAndRemove) {
    constexpr int kMainCapacity = 64;
    constexpr int kHistoryCapacity = 128;
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    constexpr int kOpsPerThread = 2000;

    LruKCache<int, int> cache(kMainCapacity, kHistoryCapacity);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
            for (int i = 0; i < kOpsPerThread; ++i) {
                int key = static_cast<int>(rng() % kKeys);
                int op = static_cast<int>(rng() % 10);
                if (op < 6) {
                    cache.put(key, key * 10);
                } else if (op < 9) {
                    int value = 0;
                    // 所有线程对同一 key 写入的值相同，命中时必须一致
                    if (cache.get(key, value)) {
                        EXPECT_EQ(value, key * 10);
                    }
                } else {
                    cache.remove(key);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_LE(cache.size(), static_cast<size_t>(kMainCapacity));
}

// ============ LeaveCallback ============

// 回调事件记录器：内部带锁，兼容多线程回调
struct LeaveRecord {
    int key;
    int value;
    LeaveReason reason;
};

class LeaveRecorder {
   public:
    explicit LeaveRecorder(LruKCache<int, int>& cache) {
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

// 晋升挤掉主缓存条目：Evicted 通知携带被淘汰条目的 key/value
TEST(LruKCacheLeaveCallbackTest, PromotionEvictionNotifiesEvicted) {
    LruKCache<int, int> cache(1, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // 1 晋升，main {1}

    cache.put(2, 20);
    cache.put(2, 21);  // 2 晋升，挤掉 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 11);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// remove 命中主缓存：Explicit 通知携带被移除条目的 key/value
TEST(LruKCacheLeaveCallbackTest, RemoveFromMainNotifiesExplicit) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // 晋升

    cache.remove(1);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 11);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// remove 只命中历史队列：对外是 no-op，不产生通知
TEST(LruKCacheLeaveCallbackTest, RemoveHistoryOnlyIsSilent) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);  // 只进历史队列

    cache.remove(1);

    EXPECT_TRUE(recorder.take().empty());
}

// remove 不存在的 key：不产生通知
TEST(LruKCacheLeaveCallbackTest, RemoveNonexistentIsSilent) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);

    cache.remove(99);

    EXPECT_TRUE(recorder.take().empty());
}

// 历史队列容量淘汰、以及晋升时历史条目的移除：均不产生通知
TEST(LruKCacheLeaveCallbackTest, HistoryEvictionAndPromotionAreSilent) {
    LruKCache<int, int> cache(2, 1);  // 历史容量 1
    LeaveRecorder recorder(cache);
    cache.put(1, 10);  // history {1}
    cache.put(2, 20);  // history 满，静默淘汰 1
    cache.put(2, 21);  // 2 晋升：历史条目移除本身不通知

    EXPECT_TRUE(recorder.take().empty());
}

// put 更新主缓存已有 key：值更新但无淘汰，不产生通知
TEST(LruKCacheLeaveCallbackTest, UpdateMainKeyIsSilent) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // 晋升，main {1}

    cache.put(1, 12);  // 纯更新

    EXPECT_TRUE(recorder.take().empty());
}

// 首次 put / 未命中 get：不产生通知
TEST(LruKCacheLeaveCallbackTest, PlainPutAndMissGetAreSilent) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    int value = 0;
    cache.get(99, value);

    EXPECT_TRUE(recorder.take().empty());
}

// get 路径的晋升也会触发主缓存淘汰：Evicted 通知
TEST(LruKCacheLeaveCallbackTest, GetPathPromotionEvictionNotifiesEvicted) {
    LruKCache<int, int> cache(1, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // main {1}

    cache.put(2, 20);  // history {2}
    int value = 0;
    cache.get(2, value);  // 晋升 2，挤掉 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 11);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.get(2), 20);  // get 晋升携带的是历史条目里的值
}

// 主缓存淘汰遵循 LRU 顺序，通知顺序与淘汰顺序一致
TEST(LruKCacheLeaveCallbackTest, EvictionsFollowRecencyOrder) {
    LruKCache<int, int> cache(2, 8);
    LeaveRecorder recorder(cache);
    for (int i = 1; i <= 2; ++i) {
        cache.put(i, i * 10);
        cache.put(i, i * 10 + 1);  // 晋升
    }  // main {1, 2}

    cache.put(3, 30);
    cache.put(3, 31);  // 挤掉 1
    cache.put(4, 40);
    cache.put(4, 41);  // 挤掉 2

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(std::make_tuple(records[0].key, records[0].value), std::make_tuple(1, 11));
    EXPECT_EQ(std::make_tuple(records[1].key, records[1].value), std::make_tuple(2, 21));
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// 一次操作只分发本轮事件：put 内部晋升产生的通知不会残留到后续操作
TEST(LruKCacheLeaveCallbackTest, EventsDoNotLeakBetweenOperations) {
    LruKCache<int, int> cache(1, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // main {1}

    cache.put(2, 20);
    cache.put(2, 21);  // 挤掉 1，应产生 1 条 Evicted

    EXPECT_EQ(recorder.take().size(), 1u);

    // 后续无关操作不应收到上轮残留事件
    cache.put(3, 30);
    cache.put(3, 31);  // 挤掉 2，产生新事件
    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 2);
}

// remove 事件与淘汰事件的 reason 正确区分
TEST(LruKCacheLeaveCallbackTest, MixedRemoveAndEvictionReportCorrectReasons) {
    LruKCache<int, int> cache(2, 4);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);
    cache.put(2, 20);
    cache.put(2, 21);  // main {1, 2}

    cache.remove(1);  // Explicit
    cache.put(3, 30);
    cache.put(3, 31);  // main {2, 3}，未满不淘汰
    cache.put(4, 40);
    cache.put(4, 41);  // 挤掉 2，Evicted

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
    EXPECT_EQ(records[1].key, 2);
    EXPECT_EQ(records[1].value, 21);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// 回调内重入 put/get/size：事件在锁外分发，安全无死锁
TEST(LruKCacheLeaveCallbackTest, ReentrantCallsInsideCallback) {
    LruKCache<int, int> cache(2, 4);
    cache.put(1, 10);
    cache.put(1, 11);
    cache.put(2, 20);
    cache.put(2, 21);  // main {1, 2}

    cache.setLeaveCallback([&cache](const int& key, const int&, LeaveReason) {
        // 回调执行时数据结构已是最终状态，size 反映移除后的结果
        EXPECT_EQ(cache.size(), 1u);
        EXPECT_EQ(cache.get(key), 0);  // 已离开的 key 不可再见
    });
    cache.remove(1);

    // 恢复记录器并验证回调确实执行过
    LeaveRecorder recorder2(cache);
    cache.remove(2);
    EXPECT_EQ(recorder2.take().size(), 1u);
}

// 晋升值经回调搬运后，嵌套操作（淘汰→再晋升）的事件链保持正确
TEST(LruKCacheLeaveCallbackTest, EvictionChainReportsEachVictim) {
    LruKCache<int, int> cache(1, 8);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // main {1}
    cache.put(2, 20);
    cache.put(2, 21);  // 挤掉 1，main {2}

    cache.put(1, 12);  // 1 历史计数已丢，重新进历史
    cache.put(1, 13);  // 再次晋升，挤掉 2
    cache.put(3, 30);  // 3 进历史
    int value = 0;
    cache.get(3, value);  // 晋升 3，挤掉 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].key, 1);  // 第一次晋升挤掉
    EXPECT_EQ(records[0].value, 11);
    EXPECT_EQ(records[1].key, 2);  // 第二次晋升挤掉
    EXPECT_EQ(records[1].value, 21);
    EXPECT_EQ(records[2].key, 1);  // get 晋升挤掉
    EXPECT_EQ(records[2].value, 13);
}

// 字符串类型端到端：key/value 类型泛化
TEST(LruKCacheLeaveCallbackTest, WorksWithStringKeyAndValue) {
    LruKCache<std::string, std::string> cache(1, 4);
    std::vector<std::pair<std::string, std::string>> records;
    cache.setLeaveCallback([&records](const std::string& k, const std::string& v, LeaveReason r) {
        records.push_back({k + ":" + v, r == LeaveReason::Evicted ? "E" : "R"});
    });

    cache.put("a", "1");
    cache.put("a", "2");  // 晋升
    cache.put("b", "3");
    cache.put("b", "4");  // 挤掉 a

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].first, "a:2");
    EXPECT_EQ(records[0].second, "E");
}

// 并发 put 压力下回调事件一致：值恒为 key*10、reason 恒为 Evicted、容量不被突破
TEST(LruKCacheLeaveCallbackTest, ConcurrentCallbackReportsConsistentEvents) {
    constexpr int kMainCapacity = 16;
    constexpr int kHistoryCapacity = 32;
    constexpr int kThreads = 8;
    constexpr int kKeys = 64;
    constexpr int kOpsPerThread = 1000;

    LruKCache<int, int> cache(kMainCapacity, kHistoryCapacity);
    std::vector<LeaveRecord> allRecords;
    std::mutex recordsMutex;
    cache.setLeaveCallback([&](const int& k, const int& v, LeaveReason r) {
        std::lock_guard<std::mutex> lock(recordsMutex);
        allRecords.push_back({k, v, r});
    });

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 1u);
            for (int i = 0; i < kOpsPerThread; ++i) {
                int key = static_cast<int>(rng() % kKeys);
                cache.put(key, key * 10);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // 所有线程对同一 key 只写 key*10：通知中的值必须一致，无撕裂值
    for (const auto& r : allRecords) {
        EXPECT_EQ(r.value, r.key * 10);
        EXPECT_EQ(r.reason, LeaveReason::Evicted);
    }
    EXPECT_FALSE(allRecords.empty());
    EXPECT_LE(cache.size(), static_cast<size_t>(kMainCapacity));
}

// ============ exists / capacity ============

// exists 仅反映主缓存；历史队列中的条目尚未驻留，不算存在
TEST(LruKCacheTest, ExistsOnlyReflectsMainCache) {
    LruKCache<int, int> cache(2, 4);

    cache.put(1, 10);  // 访问 1 次，留在历史队列
    EXPECT_FALSE(cache.exists(1));

    cache.put(1, 11);  // 达到 k 次，晋升主缓存
    EXPECT_TRUE(cache.exists(1));

    EXPECT_FALSE(cache.exists(99));
}

// exists 是 peek 语义：不影响历史计数，不会让条目提前晋升
TEST(LruKCacheTest, ExistsDoesNotAffectPromotion) {
    LruKCache<int, int> cache(1, 4);

    cache.put(1, 10);  // 访问 1 次，留在历史队列
    EXPECT_FALSE(cache.exists(1));
    EXPECT_FALSE(cache.exists(1));
    EXPECT_FALSE(cache.exists(1));  // 多次查询不累计访问次数

    cache.put(1, 11);  // 第 2 次 put 才晋升
    EXPECT_TRUE(cache.exists(1));

    cache.put(2, 20);
    cache.put(2, 21);  // 晋升 2，挤掉 1
    EXPECT_FALSE(cache.exists(1));
    EXPECT_TRUE(cache.exists(2));
}

// capacity 返回主缓存容量（构造参数），与历史队列容量无关
TEST(LruKCacheTest, CapacityReturnsMainCacheCapacity) {
    LruKCache<int, int> cache(3, 8);
    EXPECT_EQ(cache.capacity(), 3u);
}

}  // namespace
