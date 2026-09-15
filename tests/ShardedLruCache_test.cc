#include "LRU/ShardedLruCache.h"

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
using louis::cache::ShardedLruCache;

// 恒等哈希：sliceIndex = key % sliceNum，分片路由完全可控
struct IdentityHash {
    size_t operator()(int key) const { return static_cast<size_t>(key); }
};

// ============ 基础行为 ============

// put/get/remove/size 基本语义
TEST(ShardedLruCacheTest, PutGetRemoveBasics) {
    ShardedLruCache<int, int, IdentityHash> cache(16, 4);
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
TEST(ShardedLruCacheTest, MissLeavesOutParamUntouched) {
    ShardedLruCache<int, int, IdentityHash> cache(16, 4);

    int value = 42;
    EXPECT_FALSE(cache.get(99, value));
    EXPECT_EQ(value, 42);
}

// 值语义 get：命中返回值，未命中返回默认值
TEST(ShardedLruCacheTest, ValueGetOverload) {
    ShardedLruCache<int, int, IdentityHash> cache(16, 4);
    cache.put(1, 10);

    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(99), 0);
}

// 单分片退化为普通 LRU：容量 2，淘汰最旧
TEST(ShardedLruCacheTest, SingleSliceBehavesLikePlainLru) {
    ShardedLruCache<int, int, IdentityHash> cache(2, 1);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // 淘汰 1

    EXPECT_EQ(cache.get(1), 0);
    EXPECT_EQ(cache.get(2), 20);
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.size(), 2u);
}

// 默认参数构造（std::hash + hardware_concurrency 分片数）正常工作
TEST(ShardedLruCacheTest, DefaultSliceNumWorks) {
    ShardedLruCache<int, int> cache(64);
    for (int i = 0; i < 100; ++i) {
        cache.put(i, i * 10);
    }

    EXPECT_LE(cache.size(), 64u);
    EXPECT_EQ(cache.get(99), 990);
}

// ============ 精确分摊与分片独立淘汰 ============

// 精确分摊：capacity=10、4 片 → 各片容量 3,3,2,2，恰好装下 10 条不淘汰
// 恒等哈希下 key%4：{0,4,8}->片0 {1,5,9}->片1 {2,6}->片2 {3,7}->片3
TEST(ShardedLruCacheTest, PreciseCapacitySplit) {
    ShardedLruCache<int, int, IdentityHash> cache(10, 4);
    for (int key = 0; key < 10; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 10u);
    for (int key = 0; key < 10; ++key) {
        EXPECT_EQ(cache.get(key), key * 10) << "key=" << key;
    }
}

// 分片独立淘汰：只有目标分片满才淘汰，且只淘汰该分片内的 LRU
TEST(ShardedLruCacheTest, SliceEvictsIndependently) {
    ShardedLruCache<int, int, IdentityHash> cache(10, 4);
    for (int key = 0; key < 10; ++key) {
        cache.put(key, key * 10);
    }  // 各片恰好装满

    cache.put(10, 100);  // 10%4=2，片 2 淘汰其 LRU（key 2）

    EXPECT_EQ(cache.size(), 10u);
    EXPECT_EQ(cache.get(2), 0);     // 片 2 的 LRU 被淘汰
    EXPECT_EQ(cache.get(6), 60);    // 片 2 幸存
    EXPECT_EQ(cache.get(10), 100);  // 新条目存在
    EXPECT_EQ(cache.get(0), 0);     // 其他分片不受影响
    EXPECT_EQ(cache.get(1), 10);
    EXPECT_EQ(cache.get(3), 30);
    EXPECT_EQ(cache.get(7), 70);
    EXPECT_EQ(cache.get(9), 90);
}

// 总量不变量：200 个 key 挤进 capacity=100，终态恰好 100 条
TEST(ShardedLruCacheTest, TotalCapacityInvariant) {
    ShardedLruCache<int, int, IdentityHash> cache(100, 4);
    for (int key = 0; key < 200; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 100u);
}

// 分片数超过容量时被钳制为 capacity：capacity=10、请求 20 片 → 10 片各容量 1
TEST(ShardedLruCacheTest, SliceNumClampedToCapacity) {
    ShardedLruCache<int, int, IdentityHash> cache(10, 20);
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
TEST(ShardedLruCacheTest, ZeroCapacityStoresNothing) {
    ShardedLruCache<int, int, IdentityHash> cache(0, 4);
    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get(1), 0);
}

// 非法分片数（0）归 1：退化为普通 LRU，功能完整
TEST(ShardedLruCacheTest, NonPositiveSliceNumFallsBackToSingleSlice) {
    ShardedLruCache<int, int, IdentityHash> cache(8, 0);
    for (int key = 0; key < 8; ++key) {
        cache.put(key, key * 10);
    }

    EXPECT_EQ(cache.size(), 8u);
    EXPECT_EQ(cache.get(7), 70);
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
TEST(ShardedLruCacheLeaveCallbackTest, EvictionNotifiesEvicted) {
    ShardedLruCache<int, int, IdentityHash> cache(2, 1);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);
    cache.put(2, 20);

    cache.put(3, 30);  // 淘汰 1

    auto records = recorder.take();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, 1);
    EXPECT_EQ(records[0].value, 10);
    EXPECT_EQ(records[0].reason, LeaveReason::Evicted);
}

// remove 命中：Explicit 通知携带被移除条目的 key/value
TEST(ShardedLruCacheLeaveCallbackTest, RemoveNotifiesExplicit) {
    ShardedLruCache<int, int, IdentityHash> cache(4, 2);
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
TEST(ShardedLruCacheLeaveCallbackTest, RemoveNonexistentIsSilent) {
    ShardedLruCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);

    cache.remove(99);

    EXPECT_TRUE(recorder.take().empty());
}

// put 更新已有 key：不产生通知
TEST(ShardedLruCacheLeaveCallbackTest, UpdateExistingKeyIsSilent) {
    ShardedLruCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);
    cache.put(1, 10);

    cache.put(1, 11);  // 纯更新

    EXPECT_TRUE(recorder.take().empty());
}

// 未命中 get：不产生通知
TEST(ShardedLruCacheLeaveCallbackTest, PlainGetDoesNotNotify) {
    ShardedLruCache<int, int, IdentityHash> cache(8, 2);
    LeaveRecorder recorder(cache);

    int value = 0;
    cache.get(99, value);

    EXPECT_TRUE(recorder.take().empty());
}

// 回调内重入 get/put/size：分片事件在自身锁外分发，安全无死锁
TEST(ShardedLruCacheLeaveCallbackTest, CallbackCanCallCacheInsideCallback) {
    ShardedLruCache<int, int, IdentityHash> cache(4, 2);
    cache.put(0, 0);
    cache.put(2, 20);  // 片 0 满 {0, 2}
    cache.put(1, 10);  // 片 1 {1}

    cache.setLeaveCallback([&cache](const int& key, const int&, LeaveReason) {
        // 回调执行时数据结构已是最终状态：片 0 {2, 4}、片 1 {1}
        EXPECT_EQ(cache.size(), 3u);
        EXPECT_EQ(cache.get(key), 0);  // 已离开的 key 不可再见
        EXPECT_EQ(cache.get(4), 40);   // 触发淘汰的 put 已完成
        cache.put(3, 30);              // 重入写其他分片
    });
    cache.put(4, 40);  // 片 0 淘汰 0

    // 回调内的重入 put 生效
    EXPECT_EQ(cache.get(3), 30);
}

// 字符串类型端到端：默认 std::hash，key/value 类型泛化
TEST(ShardedLruCacheLeaveCallbackTest, WorksWithStringKeyAndValue) {
    ShardedLruCache<std::string, std::string> cache(1, 1);
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

// ============ 并发 ============

// 多线程混合 put/get/remove：命中值一致且总量不突破 capacity
TEST(ShardedLruCacheTest, ConcurrentPutGetRemove) {
    constexpr int kCapacity = 100;
    constexpr int kSliceNum = 4;
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    constexpr int kOpsPerThread = 2000;

    ShardedLruCache<int, int, IdentityHash> cache(kCapacity, kSliceNum);
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
TEST(ShardedLruCacheLeaveCallbackTest, ConcurrentCallbackReportsConsistentEvents) {
    constexpr int kCapacity = 100;
    constexpr int kSliceNum = 4;
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    constexpr int kOpsPerThread = 1000;

    ShardedLruCache<int, int, IdentityHash> cache(kCapacity, kSliceNum);
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
