#include "LRU/LruKCache.h"

#include <gtest/gtest.h>

#include <random>
#include <thread>
#include <vector>

namespace {

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

}  // namespace
