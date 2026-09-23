#include "ARC/ArcCache.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using louis::cache::ArcCache;

// ============ 基础 put / get / remove ============

// 基本的 put / get 功能
TEST(ArcCacheTest, PutAndGetValue) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);
    cache.put(2, 20);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
    EXPECT_EQ(cache.size(), 2u);
}

// 未命中时返回 false，且不修改传出参数
TEST(ArcCacheTest, GetMissReturnsFalseAndKeepsValue) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);

    int value = -1;
    EXPECT_FALSE(cache.get(99, value));
    EXPECT_EQ(value, -1);
}

// 值语义的 get 未命中时返回 Value 的默认值
TEST(ArcCacheTest, GetByValueReturnsDefaultOnMiss) {
    ArcCache<int, int> cache(4);
    EXPECT_EQ(cache.get(42), 0);
}

// put 已存在的 key 会更新值，且不增加条目数
TEST(ArcCacheTest, PutUpdatesExistingValue) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);
    cache.put(1, 99);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 99);
    EXPECT_EQ(cache.size(), 1u);
}

// 容量 0：put 直接丢弃，size 恒为 0，get 恒未命中，remove 不崩溃
TEST(ArcCacheTest, ZeroCapacityDropsAllPuts) {
    ArcCache<int, int> cache(0);
    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_EQ(cache.size(), 0u);
    int value = -1;
    EXPECT_FALSE(cache.get(1, value));
    EXPECT_EQ(value, -1);
    cache.remove(1);
    EXPECT_EQ(cache.size(), 0u);
}

// 容量 1：始终只驻留一条，且重复 put 不会因晋升路径丢失条目
TEST(ArcCacheTest, CapacityOneHoldsSingleEntry) {
    ArcCache<int, int> cache(1);
    cache.put(1, 10);
    EXPECT_EQ(cache.size(), 1u);
    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);

    cache.put(2, 20);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.get(1, value));
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);

    // 更新已存在的条目：容量 1 下无晋升空间，原地更新
    cache.put(2, 21);
    EXPECT_EQ(cache.size(), 1u);
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 21);

    cache.remove(2);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.get(2, value));
}

// remove 删除指定条目（无论其位于哪个部分）
TEST(ArcCacheTest, RemoveErasesEntry) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.remove(1);

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
    EXPECT_EQ(cache.size(), 1u);
}

// remove 不存在的 key 不崩溃，也不影响其他条目
TEST(ArcCacheTest, RemoveNonexistentKeyIsNoop) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);

    cache.remove(99);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    EXPECT_EQ(cache.size(), 1u);
}

// remove 腾出的空间供后续插入使用，不会引发连锁淘汰
TEST(ArcCacheTest, RemoveFreesCapacity) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.remove(1);

    cache.put(3, 30);  // 未达驻留上限，不应淘汰 2

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
    EXPECT_EQ(cache.size(), 2u);
}

// remove 后重新插入同名 key，值以最新一次 put 为准
TEST(ArcCacheTest, PutAfterRemoveReinserts) {
    ArcCache<int, int> cache(4);
    cache.put(1, 10);
    int value = 0;
    ASSERT_TRUE(cache.get(1, value));

    cache.remove(1);
    cache.put(1, 11);

    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    EXPECT_EQ(cache.size(), 1u);
}

// 非整型 Key/Value 类型
TEST(ArcCacheTest, WorksWithStringKeyAndValue) {
    ArcCache<std::string, std::string> cache(4);
    cache.put("a", "alpha");
    cache.put("b", "beta");
    cache.put("c", "gamma");  // 驻留上限 2，最早插入的 "a" 被淘汰
    EXPECT_EQ(cache.size(), 2u);

    std::string value;
    ASSERT_TRUE(cache.get("b", value));
    EXPECT_EQ(value, "beta");
    ASSERT_TRUE(cache.get("c", value));
    EXPECT_EQ(value, "gamma");
    // 注意：此处故意不查询 "a"。未命中会命中幽灵缓存 B1 并增大 p，
    // 使 T2 目标容量收缩，可能在后续晋升时连带淘汰已驻留条目，
    // 该语义由幽灵缓存相关的专项用例覆盖
}

// ============ 容量约束 ============

// 混合 put/get/remove 序列下，驻留条目数恒不超过总容量
// 键空间大于容量，会反复触发淘汰与幽灵缓存命中
TEST(ArcCacheTest, SizeNeverExceedsCapacityUnderMixedOps) {
    const size_t cap = 6;
    ArcCache<int, int> cache(cap);
    for (int i = 0; i < 3000; ++i) {
        const int k = i % 12;
        switch (i % 3) {
            case 0:
                cache.put(k, i);
                break;
            case 1: {
                int value = 0;
                cache.get(k, value);
                break;
            }
            default:
                cache.remove(k);
                break;
        }
        ASSERT_LE(cache.size(), cap);
    }
}

// ============ 两个部分各自的淘汰语义 ============
// 通过 transformThreshold 控制条目去向：
// 阈值极大 -> 条目始终留在 T1（LRU 部分，只看近期性）
// 阈值 1   -> 条目立即晋升到 T2（LFU 部分，看频次）

// T1 按近期性淘汰：老条目即使访问频次极高也会被淘汰
TEST(ArcCacheTest, RecencyPartEvictsByRecencyNotFrequency) {
    ArcCache<int, int> cache(4, 1000);  // 阈值极大，条目不晋升
    cache.put(1, 10);

    int value = 0;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(cache.get(1, value));  // 频次抬高，但仍在 T1
    }

    cache.put(2, 20);
    cache.put(3, 30);  // T1 满，按 LRU 淘汰最久未动的 1（若按频次应淘汰 2）

    EXPECT_EQ(cache.size(), 2u);
    ASSERT_TRUE(cache.get(2, value));
    EXPECT_EQ(value, 20);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
    EXPECT_FALSE(cache.get(1, value));  // 放最后：会命中幽灵缓存并调整容量
}

// T2 按频次淘汰：频次低的先出，即使它更新
TEST(ArcCacheTest, FrequencyPartEvictsByFrequencyNotRecency) {
    ArcCache<int, int> cache(4, 1);  // 阈值 1，条目立即晋升 T2
    cache.put(1, 10);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));  // 1 频次升到 2

    cache.put(2, 20);  // 2 频次 1，比 1 新
    cache.put(3, 30);  // T2 满，按频次淘汰 2（若按 LRU 应淘汰更老的 1）

    EXPECT_EQ(cache.size(), 2u);
    ASSERT_TRUE(cache.get(1, value));  // 老条目因高频存活
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
    EXPECT_FALSE(cache.get(2, value));  // 放最后：会命中幽灵缓存并调整容量
}

// 达到转换阈值后条目晋升到 T2：此后 T1 反复淘汰腾挪，高访条目稳定驻留
TEST(ArcCacheTest, ReachingThresholdPromotesToFrequencyPart) {
    ArcCache<int, int> cache(4);  // 默认阈值 2
    cache.put(1, 10);
    cache.put(1, 11);  // 第二次访问达到阈值，晋升到 T2

    // 一连串只访问一次的新键在 T1 中反复淘汰
    for (int k = 2; k <= 8; ++k) {
        cache.put(k, k * 10);
    }

    EXPECT_EQ(cache.size(), 3u);
    int value = 0;
    ASSERT_TRUE(cache.get(1, value));  // 晋升后不受 T1 淘汰影响
    EXPECT_EQ(value, 11);
    ASSERT_TRUE(cache.get(8, value));
    EXPECT_EQ(value, 80);
    EXPECT_FALSE(cache.get(2, value));  // 早已被逐出主缓存与幽灵缓存
}

// ============ 幽灵缓存与自适应容量 ============

// 条目被淘汰后重新 put：可正常恢复，值以最新一次 put 为准
TEST(ArcCacheTest, GhostHitReinsertRestoresEntry) {
    ArcCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);  // 驻留上限 1，1 被淘汰入幽灵缓存

    int value = 0;
    EXPECT_FALSE(cache.get(1, value));  // 幽灵缓存命中不算命中

    cache.put(1, 11);  // 幽灵命中后重新插入
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.get(2, value));
}

// 命中 B1（因“不够新”被淘汰）-> 倾向近期性，T1 目标容量增大，
// 表现为重新插入时不再挤掉现有条目，驻留数增长
TEST(ArcCacheTest, RecencyGhostHitGrowsLruPart) {
    ArcCache<int, int> cache(4, 1000);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    cache.put(4, 40);  // T1 目标容量 2，仅 {3,4} 驻留，{1,2} 进幽灵缓存
    EXPECT_EQ(cache.size(), 2u);

    cache.put(1, 11);  // B1 命中 -> p 增大 -> T1 容纳 3 条，无需淘汰
    EXPECT_EQ(cache.size(), 3u);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
    ASSERT_TRUE(cache.get(4, value));
    EXPECT_EQ(value, 40);
    EXPECT_FALSE(cache.get(2, value));  // 放最后：会命中幽灵缓存并调整容量
}

// 命中 B2（因“不够频繁”被淘汰）-> 倾向频次，T2 目标容量增大，
// 表现为重新插入后驻留数增长
TEST(ArcCacheTest, FrequencyGhostHitGrowsLfuPart) {
    ArcCache<int, int> cache(4, 1);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    cache.put(4, 40);  // T2 目标容量 2，仅 {3,4} 驻留，{1,2} 进幽灵缓存
    EXPECT_EQ(cache.size(), 2u);

    cache.put(1, 11);  // B2 命中 -> p 减小 -> T2 容纳 3 条
    EXPECT_EQ(cache.size(), 3u);

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 11);
    ASSERT_TRUE(cache.get(3, value));
    EXPECT_EQ(value, 30);
    ASSERT_TRUE(cache.get(4, value));
    EXPECT_EQ(value, 40);
    EXPECT_FALSE(cache.get(2, value));  // 放最后：会命中幽灵缓存并调整容量
}

// ============ 并发 ============

// 多线程并发 put/get，验证线程安全且驻留不超容量
TEST(ArcCacheTest, ConcurrentPutAndGet) {
    constexpr int kCapacity = 64;
    constexpr int kThreads = 8;
    constexpr int kKeys = 1000;
    constexpr int kOpsPerThread = 2000;

    ArcCache<int, int> cache(kCapacity);
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

// 多线程混合 put/get/remove，进一步覆盖 remove 路径
TEST(ArcCacheTest, ConcurrentMixedOpsWithRemove) {
    constexpr int kCapacity = 64;
    constexpr int kThreads = 4;
    constexpr int kKeys = 128;
    constexpr int kOpsPerThread = 2000;

    ArcCache<int, int> cache(kCapacity);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            std::mt19937 rng(static_cast<unsigned>(t) * 104729u + 7u);
            for (int i = 0; i < kOpsPerThread; ++i) {
                int key = static_cast<int>(rng() % kKeys);
                switch (rng() % 3) {
                    case 0:
                        cache.put(key, key * 2);
                        break;
                    case 1: {
                        int value = 0;
                        if (cache.get(key, value)) {
                            EXPECT_EQ(value, key * 2);
                        }
                        break;
                    }
                    default:
                        cache.remove(key);
                        break;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_LE(cache.size(), static_cast<size_t>(kCapacity));
}

}  // namespace
