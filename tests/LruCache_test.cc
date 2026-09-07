#include "LRU/LruCache.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

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

}  // namespace
