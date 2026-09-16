#include "LFU/ShardedLfuCache.h"

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
using louis::cache::ShardedLfuCache;

// 恒等哈希：sliceIndex = key % sliceNum，分片路由完全可控
struct IdentityHash {
    size_t operator()(int key) const { return static_cast<size_t>(key); }
};

// ============ 基础行为 ============

// put/get/remove/size 基本语义
TEST(ShardedLfuCacheTest, PutGetRemoveBasics) {
    ShardedLfuCache<int, int, IdentityHash> cache(16, 4);
    cache.put(1, 10);
    cache.put(5, 50);  // 与 1 同属分片 1

    int value = 0;
    ASSERT_TRUE(cache.get(1, value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(cache.get(5, value));
    EXPECT_EQ(value, 50);
    EXPECT_EQ(cache.size(), 2u);

    cache.remove(1);
    EXPECT_FALSE(cache.get(1, value));
    EXPECT_EQ(cache.size(), 1u);
}

// 未命中不修改传出参数
TEST(ShardedLfuCacheTest, MissLeavesOutParamUntouched) {
    ShardedLfuCache<int, int, IdentityHash> cache(16, 4);

    int value = 42;
    EXPECT_FALSE(cache.get(99, value));
    EXPECT_EQ(value, 42);
}

// 值语义 get：命中返回值，未命中返回默认值
TEST(ShardedLfuCacheTest, ValueGetOverload) {
    ShardedLfuCache<int, int, IdentityHash> cache(16, 4);
    cache.put(1, 10);

    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(99), 0);
}

// put 更新已有 key：值被覆盖，不增加条目数
TEST(ShardedLfuCacheTest, PutUpdatesExistingKey) {
    ShardedLfuCache<int, int, IdentityHash> cache(16, 4);
    cache.put(1, 10);
    cache.put(1, 99);

    EXPECT_EQ(cache.get(1), 99);
    EXPECT_EQ(cache.size(), 1u);
}

// 单分片退化为普通 LFU：淘汰频次最低者
TEST(ShardedLfuCacheTest, SingleSliceBehavesLikePlainLfu) {
    ShardedLfuCache<int, int, IdentityHash> cache(2, 1);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.get(1);  // 抬高 1 的频次
    cache.get(1);

    cache.put(3, 30);  // 淘汰频次最低的 2

    EXPECT_EQ(cache.get(2), 0);
    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.size(), 2u);
}

// 默认参数构造（std::hash + hardware_concurrency 分片数）正常工作
TEST(ShardedLfuCacheTest, DefaultSliceNumWorks) {
    ShardedLfuCache<int, int> cache(64);
    for (int i = 0; i < 100; ++i) {
        cache.put(i, i * 10);
    }

    EXPECT_LE(cache.size(), 64u);
    // 同频次按插入顺序淘汰，最后插入的 key 必然驻留
    EXPECT_EQ(cache.get(99), 990);
}

// ============ 精确分摊与分片独立淘汰 ============

// 精确分摊：capacity=10、4 片 → 各片容量 3,3,2,2，恰好装下 10 条不淘汰
// 恒等哈希下 key%4：{0,4,8}->片0 {1,5,9}->片1 {2,6}->片2 {3,7}->片3
TEST(ShardedLfuCacheTest, PreciseCapacitySplit) {
    ShardedLfuCache<int, int, IdentityHash> cache(10, 4);
    for (int key = 0; key < 10; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 10u);
    for (int key = 0; key < 10; ++key) {
        EXPECT_EQ(cache.get(key), key * 10) << "key=" << key;
    }
}

// 分片独立淘汰：只有目标分片满才淘汰，且只淘汰该分片内频次最低者
TEST(ShardedLfuCacheTest, SliceEvictsIndependently) {
    ShardedLfuCache<int, int, IdentityHash> cache(10, 4);
    for (int key = 0; key < 10; ++key) {
        cache.put(key, key * 10);
    }  // 各片恰好装满

    cache.put(10, 100);  // 10%4=2，片 2 淘汰其最低频次条目（同频次则最早插入的 key 2）

    EXPECT_EQ(cache.size(), 10u);
    EXPECT_EQ(cache.get(2), 0);     // 片 2 的淘汰者
    EXPECT_EQ(cache.get(6), 60);    // 片 2 幸存
    EXPECT_EQ(cache.get(10), 100);  // 新条目存在
    EXPECT_EQ(cache.get(0), 0);     // 其他分片不受影响
    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.get(7), 70);
    EXPECT_EQ(cache.get(9), 90);
}

// 分片淘汰优先级是“频次最低”：高频条目在同分片内免于淘汰
TEST(ShardedLfuCacheTest, SliceEvictionRespectsFrequency) {
    ShardedLfuCache<int, int, IdentityHash> cache(4, 2);  // 每片容量 2
    cache.put(0, 0);
    cache.put(2, 20);  // 片 0 满 {0, 2}

    cache.get(0);  // 抬高 0 的频次

    cache.put(4, 40);  // 片 0 淘汰频次最低的 2

    EXPECT_EQ(cache.get(0), 0);  // 热条目幸存
    EXPECT_EQ(cache.get(2), 0);  // 冷条目被淘汰
    EXPECT_EQ(cache.get(4), 40);
    EXPECT_EQ(cache.size(), 2u);  // 全部落在片 0，片 1 仍为空
}

// 频次保护是按分片独立的：抬高一个分片的频次不影响另一个分片的淘汰
TEST(ShardedLfuCacheTest, FrequencyProtectionIsPerSlice) {
    ShardedLfuCache<int, int, IdentityHash> cache(4, 2);  // 每片容量 2
    cache.put(0, 0);
    cache.put(2, 20);  // 片 0 {0, 2}
    cache.put(1, 10);
    cache.put(3, 30);  // 片 1 {1, 3}

    cache.get(0);
    cache.get(0);  // 片 0 中 0 变热

    cache.put(5, 50);  // 片 1 满，全部同频次 → 淘汰最早插入的 1
    cache.put(4, 40);  // 片 0 满，淘汰频次最低的 2

    EXPECT_EQ(cache.get(0), 0);  // 片 0 热条目幸存
    EXPECT_EQ(cache.get(2), 0);  // 片 0 冷条目淘汰
    EXPECT_EQ(cache.get(4), 40);
    EXPECT_EQ(cache.get(1), 0);  // 片 1 按插入顺序淘汰
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.get(5), 50);
}

// 总量不变量：200 个 key 挤进 capacity=100，终态恰好 100 条
TEST(ShardedLfuCacheTest, TotalCapacityInvariant) {
    ShardedLfuCache<int, int, IdentityHash> cache(100, 4);
    for (int key = 0; key < 200; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 100u);
}

// 分片数超过容量时被钳制为 capacity：capacity=10、请求 20 片 → 10 片各容量 1
TEST(ShardedLfuCacheTest, SliceNumClampedToCapacity) {
    ShardedLfuCache<int, int, IdentityHash> cache(10, 20);
    for (int key = 0; key < 30; ++key) {
        cache.put(key, key * 10);
    }

    // 每片容量 1，最后写入的 key 20-29（key%10 与 0-9 同片）幸存
    EXPECT_EQ(cache.size(), 10u);
    for (int key = 0; key < 20; ++key) {
        EXPECT_EQ(cache.get(key), 0) << "key=" << key;
    }
    for (int key = 20; key < 30; ++key) {
        EXPECT_EQ(cache.get(key), key * 10) << "key=" << key;
    }
}

// 容量 0：单片容量 0，什么都存不下
TEST(ShardedLfuCacheTest, ZeroCapacityStoresNothing) {
    ShardedLfuCache<int, int, IdentityHash> cache(0, 4);
    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);
}

// 非法分片数（0）归 1：退化为普通 LFU，功能完整
TEST(ShardedLfuCacheTest, NonPositiveSliceNumFallsBackToSingleSlice) {
    ShardedLfuCache<int, int, IdentityHash> cache(8, 0);
    for (int key = 0; key < 8; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 8u);
    EXPECT_EQ(cache.get(7), 70);
}

// 非法分片数（负数）归 1，不触发除零
TEST(ShardedLfuCacheTest, NegativeSliceNumFallsBackToSingleSlice) {
    ShardedLfuCache<int, int, IdentityHash> cache(4, -3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);

    EXPECT_EQ(cache.size(), 3u);
    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(3), 30);
}

// remove 腾出的空位不被后续 put 立即用于淘汰
TEST(ShardedLfuCacheTest, RemoveFreesCapacityWithinSlice) {
    ShardedLfuCache<int, int, IdentityHash> cache(2, 2);  // 每片容量 1
    cache.put(0, 0);                                      // 片 0
    cache.put(1, 10);                                     // 片 1
    cache.remove(0);                                      // 片 0 空出

    cache.put(2, 20);  // 片 0 未满，不应淘汰任何条目

    EXPECT_EQ(cache.get(0), 0);
    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(2), 20);
    EXPECT_EQ(cache.size(), 2u);
}

// ============ LeaveCallback ============

struct LeaveRecord {
    int key;
    int value;
    LeaveReason reason;
};

// 回调事件记录器：模板构造兼容不同哈希类型的缓存，内部带锁
class LeaveRecorder {
   public:
    template <typename CacheT>
    explicit LeaveRecorder(CacheT& cache) {
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

// 分片满触发淘汰：Evicted 通知携带被淘汰条目的 key/value
TEST(ShardedLfuCacheLeaveCallbackTest, EvictionNotifiesEvicted) {
    ShardedLfuCache<int, int, IdentityHash> cache(2, 1);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.put(3, 30);  // 同频次，淘汰最早插入的 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// 淘汰通知的是频次最低者，高频条目不会被选中
TEST(ShardedLfuCacheLeaveCallbackTest, EvictionNotifiesLeastFrequent) {
    ShardedLfuCache<int, int, IdentityHash> cache(2, 1);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.get(2);  // 抬高 2 的频次

    cache.put(3, 30);  // 淘汰频次最低的 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// remove 命中：Explicit 通知携带被移除条目的 key/value
TEST(ShardedLfuCacheLeaveCallbackTest, RemoveNotifiesExplicit) {
    ShardedLfuCache<int, int, IdentityHash> cache(4, 2);
    LeaveRecorder recorder(cache);
    cache.put(0, 10);  // 片 0

    cache.remove(0);

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 0);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
}

// remove 不存在的 key：不产生通知
TEST(ShardedLfuCacheLeaveCallbackTest, RemoveNonexistentIsSilent) {
    ShardedLfuCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);

    cache.remove(99);

    EXPECT_TRUE(recorder.take().empty());
}

// put 更新已有 key：不产生通知
TEST(ShardedLfuCacheLeaveCallbackTest, UpdateExistingKeyIsSilent) {
    ShardedLfuCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);

    cache.put(1, 11);  // 纯更新

    EXPECT_TRUE(recorder.take().empty());
}

// 未命中 get：不产生通知
TEST(ShardedLfuCacheLeaveCallbackTest, PlainGetDoesNotNotify) {
    ShardedLfuCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);

    int value = 0;
    cache.get(99, value);

    EXPECT_TRUE(recorder.take().empty());
}

// 容量 0：丢弃的条目未曾驻留，不产生通知
TEST(ShardedLfuCacheLeaveCallbackTest, ZeroCapacityPutIsSilent) {
    ShardedLfuCache<int, int, IdentityHash> cache(0, 4);
    LeaveRecorder recorder(cache);

    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_TRUE(recorder.take().empty());
}

// 回调内重入 get/put/size：分片事件在自身锁外分发，安全无死锁
TEST(ShardedLfuCacheLeaveCallbackTest, CallbackCanCallCacheInsideCallback) {
    ShardedLfuCache<int, int, IdentityHash> cache(4, 2);  // 每片容量 2
    cache.put(0, 0);
    cache.put(2, 20);  // 片 0 满 {0, 2}
    cache.put(1, 10);  // 片 1 {1}

    cache.setLeaveCallback([&cache](const int& key, const int&, LeaveReason) {
        // 回调执行时数据结构已是最终状态：片 0 {2, 4}、片 1 {1}
        EXPECT_EQ(cache.size(), 3u);
        EXPECT_EQ(cache.get(key), 0);  // 已离开的 key 不可再见
        EXPECT_EQ(cache.get(4), 40);   // 触发淘汰的 put 已完成
        cache.put(3, 30);              // 重入写其他分片（片 1 尚有空位）
    });
    cache.put(4, 40);  // 片 0 淘汰同频次中最早插入的 0

    // 回调内的重入 put 生效
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.size(), 4u);
}

// 字符串类型端到端：默认 std::hash，key/value 类型泛化
TEST(ShardedLfuCacheLeaveCallbackTest, WorksWithStringKeyAndValue) {
    ShardedLfuCache<std::string, std::string> cache(1, 1);
    std::vector<std::pair<std::string, std::string>> records;
    cache.setLeaveCallback([&records](const std::string& k, const std::string& v, LeaveReason r) {
        records.push_back({k + ":" + v, r == LeaveReason::Evicted ? "E" : "R"});
    });

    cache.put("a", "1");
    cache.put("b", "2");  // 淘汰 a

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].first, "a:1");
    EXPECT_EQ(records[0].second, "E");
}

// 混合 Explicit / Evicted 时 reason 区分正确
TEST(ShardedLfuCacheLeaveCallbackTest, MixedRemoveAndEvictionReportCorrectReasons) {
    ShardedLfuCache<int, int, IdentityHash> cache(2, 1);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.remove(1);   // Explicit，并腾出空位
    cache.put(3, 30);  // 未满，不淘汰
    cache.put(4, 40);  // 满，淘汰同频次中最早插入的 2

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].reason, LeaveReason::Explicit);
    EXPECT_EQ(records[1].key, 2);
    EXPECT_EQ(records[1].value, 20);
    EXPECT_EQ(records[1].reason, LeaveReason::Evicted);
}

// ============ 并发 ============

// 多线程混合 put/get/remove：命中值一致且总量不突破 capacity
TEST(ShardedLfuCacheTest, ConcurrentPutGetRemove) {
    constexpr int kCapacity = 100;
    constexpr int kSliceNum = 4;
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    constexpr int kOpsPerThread = 2000;

    ShardedLfuCache<int, int, IdentityHash> cache(kCapacity, kSliceNum);
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

    EXPECT_LE(cache.size(), static_cast<size_t>(kCapacity));
}

// 并发 put 压力下回调事件一致：值恒为 key*10、reason 恒为 Evicted
TEST(ShardedLfuCacheLeaveCallbackTest, ConcurrentCallbackReportsConsistentEvents) {
    constexpr int kCapacity = 100;
    constexpr int kSliceNum = 4;
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    constexpr int kOpsPerThread = 1000;

    ShardedLfuCache<int, int, IdentityHash> cache(kCapacity, kSliceNum);
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
    EXPECT_LE(cache.size(), static_cast<size_t>(kCapacity));
}

}  // namespace
