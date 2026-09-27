#include "ARC/ArcCache.h"

#include <gtest/gtest.h>

#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Policy.h"

namespace {

using louis::cache::ArcCache;
using louis::cache::LeaveReason;

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

// ============ LeaveCallback ============
// 语义约定：
//   1. 主缓存（T1 / T2）条目被驱逐 -> Evicted，此时条目已不可被 get 到
//   2. 显式 remove 命中主缓存 -> Explicit
//   3. 条目在缓存内部从 T1 迁移到 T2、以及幽灵缓存的自身流转 -> 不通知

// 回调事件记录器：内部带锁，兼容多线程回调
struct LeaveRecord {
    int key;
    int value;
    LeaveReason reason;
};

class LeaveRecorder {
   public:
    explicit LeaveRecorder(ArcCache<int, int>& cache) {
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

// 阈值极大使条目始终留在 T1：T1 满时按近期性驱逐并通知 Evicted
TEST(ArcCacheLeaveCallbackTest, LruPartEvictionNotifiesEvicted) {
    ArcCache<int, int> cache(4, 1000);  // T1 目标容量 2
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // T1 满，淘汰最久未动的 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_EQ(cache.size(), 2u);
}

// 阈值 1 使条目立即进入 T2：T2 满时按频次驱逐并通知 Evicted
TEST(ArcCacheLeaveCallbackTest, LfuPartEvictionNotifiesEvicted) {
    ArcCache<int, int> cache(4, 1);  // T2 目标容量 2
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // T2 满，淘汰频次最低且最旧的 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// remove 命中 T1：Explicit 通知携带被移除条目的 key/value
TEST(ArcCacheLeaveCallbackTest, RemoveFromLruPartNotifiesExplicit) {
    ArcCache<int, int> cache(4, 1000);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);

    cache.remove(1);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// remove 命中 T2：同样记为 Explicit
TEST(ArcCacheLeaveCallbackTest, RemoveFromLfuPartNotifiesExplicit) {
    ArcCache<int, int> cache(4, 1);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);  // 立即晋升到 T2

    cache.remove(1);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// remove 不存在的 key：不产生通知
TEST(ArcCacheLeaveCallbackTest, RemoveNonexistentIsSilent) {
    ArcCache<int, int> cache(4, 1000);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);

    cache.remove(99);

    EXPECT_TRUE(recorder.take().empty());
    EXPECT_EQ(cache.size(), 1u);
}

// 达到转换阈值时 T1 -> T2 的迁移只是条目在缓存内部换部分，
// 对用户而言并未离开：不产生任何通知，且条目仍可命中
TEST(ArcCacheLeaveCallbackTest, TransformBetweenPartsIsSilent) {
    ArcCache<int, int> cache(4);  // 默认阈值 2
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(1, 11);  // 第二次访问达到阈值，迁移到 T2

    EXPECT_TRUE(recorder.take().empty());

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));  // 迁移后从 T2 命中
    EXPECT_EQ(value, 11);
    EXPECT_TRUE(recorder.take().empty());
}

// 命中幽灵缓存引发容量重划分：p 增大后 T2 目标容量收缩，
// 被挤出的 T2 条目与先前 T1 驱逐的条目一并按顺序通知
TEST(ArcCacheLeaveCallbackTest, GhostHitShrinksOppositePartAndNotifies) {
    ArcCache<int, int> cache(4, 2);  // T1 / T2 目标容量均为 2
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(1, 10);  // 1 晋升到 T2
    cache.put(2, 20);
    cache.put(2, 20);  // 2 晋升到 T2，T2 满
    cache.put(3, 30);
    cache.put(4, 40);
    cache.put(5, 50);  // T1 满，3 被驱逐并进入 B1

    int value = 0;
    EXPECT_FALSE(cache.get(3, value));  // 命中 B1 -> p 增大 -> T2 收缩，挤掉 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 3);  // 先因 T1 满被驱逐
    EXPECT_EQ(records[0].value, 30);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_EQ(records[1].key, 1);  // 后因 T2 收缩被挤出
    EXPECT_EQ(records[1].value, 10);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
    EXPECT_EQ(cache.size(), 3u);
}

// remove 事件与驱逐事件的 reason 正确区分，且通知顺序与发生顺序一致
TEST(ArcCacheLeaveCallbackTest, MixedRemoveAndEvictionReportCorrectReasons) {
    ArcCache<int, int> cache(4, 1000);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.remove(1);  // Explicit
    cache.put(3, 30);
    cache.put(4, 40);  // T1 满，淘汰 2

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
    EXPECT_EQ(records[1].key, 2);
    EXPECT_EQ(records[1].value, 20);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// 一次操作只分发本轮事件：淘汰产生的通知不会残留到后续操作
TEST(ArcCacheLeaveCallbackTest, EventsDoNotLeakBetweenOperations) {
    ArcCache<int, int> cache(4, 1000);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // 淘汰 1

    EXPECT_EQ(recorder.take().size(), 1u);

    // 后续无关操作不应收到上轮残留事件
    cache.put(4, 40);  // 淘汰 2
    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 2);
    EXPECT_EQ(records[0].value, 20);
}

// 回调内重入 put/get/size：事件在锁外分发，安全无死锁
TEST(ArcCacheLeaveCallbackTest, ReentrantCallsInsideCallback) {
    ArcCache<int, int> cache(4, 1000);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.setLeaveCallback([&cache](const int& key, const int&, LeaveReason) {
        // 回调执行时数据结构已是最终状态，size 反映驱逐后的结果
        EXPECT_EQ(cache.size(), 2u);
        EXPECT_EQ(cache.get(key), 0);  // 已离开主缓存的 key 再也取不到
    });
    cache.put(3, 30);  // 淘汰 1，触发回调

    // 回调确实执行过：换成记录器后走一次 Explicit 路径
    LeaveRecorder recorder(cache);
    cache.remove(2);
    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 2);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// 字符串类型端到端：key/value 类型泛化
TEST(ArcCacheLeaveCallbackTest, WorksWithStringKeyAndValue) {
    ArcCache<std::string, std::string> cache(4, 1000);
    std::vector<std::pair<std::string, LeaveReason>> records;
    cache.setLeaveCallback([&records](const std::string& k, const std::string&, LeaveReason r) {
        records.push_back({k, r});
    });

    cache.put("a", "alpha");
    cache.put("b", "beta");
    cache.remove("a");  // Explicit
    cache.put("c", "gamma");
    cache.put("d", "delta");  // T1 满，淘汰 b

    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].first, "a");
    EXPECT_EQ(records[0].second, LeaveReason::Explicit);
    EXPECT_EQ(records[1].first, "b");
    EXPECT_EQ(records[1].second, LeaveReason::Evicted);
}

// 并发 put/get 压力下回调事件一致：值恒为 key*10、reason 恒为 Evicted、容量不被突破
TEST(ArcCacheLeaveCallbackTest, ConcurrentCallbackReportsConsistentEvents) {
    constexpr int kCapacity = 32;
    constexpr int kThreads = 8;
    constexpr int kKeys = 128;
    constexpr int kOpsPerThread = 1000;

    ArcCache<int, int> cache(kCapacity);
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
                if (rng() % 2 == 0) {
                    cache.put(key, key * 10);
                } else {
                    int value = 0;
                    cache.get(key, value);
                }
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
    EXPECT_LE(cache.size(), static_cast<size_t>(kCapacity));
}

// ============ exists / capacity ============

// exists 命中与未命中：T1 与 T2 中的条目均算存在
TEST(ArcCacheTest, ExistsReportsPresence) {
    ArcCache<int, int> cache(4);  // 默认阈值 2
    EXPECT_FALSE(cache.exists(1));

    cache.put(1, 10);
    EXPECT_TRUE(cache.exists(1));  // T1

    cache.put(1, 11);  // 达到阈值，晋升 T2
    EXPECT_TRUE(cache.exists(1));

    cache.put(2, 20);
    EXPECT_TRUE(cache.exists(2));
    EXPECT_FALSE(cache.exists(99));
}

// exists 对幽灵缓存条目返回 false，且不消耗幽灵命中：
// 若 exists 误触发幽灵命中，p 调整引发的容量重划分驱逐会提前发生在 exists 内部
TEST(ArcCacheTest, ExistsIgnoresGhostCaches) {
    ArcCache<int, int> cache(8);  // p=4：T1/T2/B1/B2 目标容量均为 4
    LeaveRecorder recorder(cache);

    // 构造：B2 = {1,2}，B1 = {7}，T2 = {3,4,5,6}，T1 = {8,9,10,11}
    cache.put(1, 10);
    cache.put(1, 11);  // 晋升 T2
    cache.put(2, 20);
    cache.put(2, 21);
    cache.put(3, 30);
    cache.put(3, 31);
    cache.put(4, 40);
    cache.put(4, 41);  // T2 {1,2,3,4}
    cache.put(5, 50);
    cache.put(5, 51);  // 挤掉 1 -> B2
    cache.put(6, 60);
    cache.put(6, 61);  // 挤掉 2 -> B2，T2 {3,4,5,6}
    cache.put(7, 70);
    cache.put(8, 80);
    cache.put(9, 90);
    cache.put(10, 100);
    cache.put(11, 110);  // T1 满，挤掉 7 -> B1，T1 {8,9,10,11}

    auto setup = recorder.take();
    ASSERT_EQ(setup.size(), 3u);  // 1、2、7 被驱逐
    EXPECT_EQ(setup[0].key, 1);
    EXPECT_EQ(setup[1].key, 2);
    EXPECT_EQ(setup[2].key, 7);

    EXPECT_TRUE(cache.exists(3));   // T2
    EXPECT_TRUE(cache.exists(11));  // T1
    EXPECT_FALSE(cache.exists(7));  // 幽灵缓存不算存在
    EXPECT_FALSE(cache.exists(99));

    // 关键断言：exists 未消耗 7 的幽灵命中
    // 之后 put(7) 命中 B1，p 增 2（δ1 = ceil(|B2|/|B1|) = 2），
    // T2 目标容量 4 -> 2，挤出最久未用的 3、4
    EXPECT_TRUE(recorder.take().empty());  // exists 本身不产生任何事件
    cache.put(7, 71);
    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 3);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
    EXPECT_EQ(records[1].key, 4);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);

    cache.put(12, 120);  // T1 目标容量 6，尚有空间，无驱逐
    EXPECT_TRUE(recorder.take().empty());

    EXPECT_EQ(cache.size(), 8u);  // 恰为总容量

    int value = 0;
    ASSERT_TRUE(cache.get(5, value));  // T2 缩容后幸存
    EXPECT_EQ(value, 51);
    ASSERT_TRUE(cache.get(6, value));  // 另一个幸存者
    EXPECT_EQ(value, 61);
    EXPECT_FALSE(cache.get(3, value)
    );  // T2 缩容牺牲者，已入幽灵缓存（会触发幽灵命中调整 p，故放后面）
    EXPECT_FALSE(cache.get(4, value));

    // 放最后：get(7) 会使其达到转换阈值迁入 T2，get(3)/get(4) 的幽灵命中也会调整 p
    ASSERT_TRUE(cache.get(7, value));
    EXPECT_EQ(value, 71);
}

// capacity 返回构造时设定的总容量，且不随 put 改变
TEST(ArcCacheTest, CapacityReturnsConfiguredValue) {
    ArcCache<int, int> cache(6);
    EXPECT_EQ(cache.capacity(), 6u);

    cache.put(1, 10);
    EXPECT_EQ(cache.capacity(), 6u);
}

// 容量 0：条目不驻留，exists 恒为 false，capacity 为 0
TEST(ArcCacheTest, ZeroCapacityExistsAlwaysFalse) {
    ArcCache<int, int> cache(0);
    cache.put(1, 10);  // 被丢弃

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.exists(1));
    EXPECT_EQ(cache.capacity(), 0u);
}

}  // namespace
