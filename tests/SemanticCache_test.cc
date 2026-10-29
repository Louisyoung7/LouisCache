#include "Semantic/SemanticCache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "LRU/LruCache.h"
#include "Policy.h"
#include "Semantic/Embedder.h"
#include "Semantic/VectorIndex.h"
#include "Ttl/TtlCache.h"

// 本文件以「契约驱动」方式覆盖 SemanticCache 的编排逻辑。
//
// 声明的契约（实现需保证）：
//  1. 精确命中不得调用 Embedder::embed，也不得访问 VectorIndex。
//  2. 语义命中：best.score >= hitThreshold 即命中，score 原样回传；
//     exactIndexEnabled == false 时不得走精确路径。
//  3. 无幽灵命中：容量淘汰 / remove / TTL 过期后，同义 query 不再命中该条目，
//     且 vectorIndex.size() 与 store.size() 稳定后必须相等。
//  4. put 的失败/竞态不得留下「有向量无条目」或「有条目无向量」的残状态。
//  5. 统计口径：exactHits / semanticHits / misses / evictions / expirations / puts 各计其数。
//  6. 所有公开方法线程安全，并发混合操作后索引与存储不脱节。

namespace {

using louis::cache::BruteForceIndex;
using louis::cache::CacheEntry;
using louis::cache::GetResult;
using louis::cache::HashEmbedder;
using louis::cache::LruCache;
using louis::cache::Options;
using louis::cache::Policy;
using louis::cache::SemanticCache;
using louis::cache::TtlCache;
using louis::cache::VectorIndex;
using louis::cache::VectorMatch;

constexpr size_t kDim = 128;  // HashEmbedder 默认维度

// 三组语义距离明确的样本，供精确 / 语义 / 无关三条路径复用
constexpr const char* kWeather = "今天天气怎么样";
constexpr const char* kWeatherVariant = "今天的天气怎么样";  // 与 kWeather 余弦约 0.87
constexpr const char* kParis = "巴黎有哪些好吃的餐厅";
constexpr const char* kUnrelated = "如何学习 C++ 模板元编程";

Options makeOptions(size_t capacity, float threshold = 0.85f, size_t searchK = 5, bool exact = true) {
    Options o;
    o.capacity = capacity;
    o.hitThreshold = threshold;
    o.searchK = searchK;
    o.exactIndexEnabled = exact;
    return o;
}

// ---------------------------------------------------------------- 测试替身

// 包装 HashEmbedder，记录 embed 调用次数
class CountingEmbedder : public louis::cache::Embedder {
   public:
    std::vector<float> embed(const std::string& text) const override {
        calls.fetch_add(1);
        return inner_.embed(text);
    }
    size_t dim() const override { return inner_.dim(); }
    std::string modelId() const override { return inner_.modelId(); }

    mutable std::atomic<int> calls{0};

   private:
    HashEmbedder inner_;
};

// 第 N 次 embed 抛异常，用于验证 put 失败后的清理
class ThrowingEmbedder : public louis::cache::Embedder {
   public:
    explicit ThrowingEmbedder(int throwOnCall) : throwOnCall_(throwOnCall) {}

    std::vector<float> embed(const std::string& text) const override {
        if (calls.fetch_add(1) + 1 == throwOnCall_) throw std::runtime_error("embed failed");
        return inner_.embed(text);
    }
    size_t dim() const override { return inner_.dim(); }
    std::string modelId() const override { return inner_.modelId(); }

    mutable std::atomic<int> calls{0};

   private:
    int throwOnCall_;
    HashEmbedder inner_;
};

// 第一次 embed 时执行注入的钩子，用于在「exists 之后、add 之前」插入竞态
class HookEmbedder : public louis::cache::Embedder {
   public:
    void setHook(std::function<void()> hook) { hook_ = std::move(hook); }

    std::vector<float> embed(const std::string& text) const override {
        if (hook_) {
            auto hook = std::move(hook_);
            hook_ = nullptr;
            hook();
        }
        return inner_.embed(text);
    }
    size_t dim() const override { return inner_.dim(); }
    std::string modelId() const override { return inner_.modelId(); }

   private:
    mutable std::function<void()> hook_{};
    HashEmbedder inner_;
};

// 任意输入都返回同一单位向量：任意两条目的余弦恒为 1，方便卡阈值边界
class ConstantEmbedder : public louis::cache::Embedder {
   public:
    std::vector<float> embed(const std::string&) const override { return {1.0f, 0.0f, 0.0f, 0.0f}; }
    size_t dim() const override { return 4; }
    std::string modelId() const override { return "constant:v1"; }
};

// 记录 add / remove / search 的调用次数
class CountingIndex : public BruteForceIndex {
   public:
    explicit CountingIndex(size_t dim) : BruteForceIndex(dim) {}

    void add(uint64_t id, const std::vector<float>& vector) override {
        adds.fetch_add(1);
        BruteForceIndex::add(id, vector);
    }
    void remove(uint64_t id) override {
        removes.fetch_add(1);
        BruteForceIndex::remove(id);
    }
    std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const override {
        searches.fetch_add(1);
        return BruteForceIndex::search(query, k);
    }

    mutable std::atomic<int> searches{0};
    std::atomic<int> adds{0};
    std::atomic<int> removes{0};
};

// 可手动拨动的测试时钟（仅单线程用例使用）
class ManualClock {
   public:
    using rep = std::chrono::milliseconds::rep;
    using period = std::chrono::milliseconds::period;
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<ManualClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() { return current(); }
    static void advance(duration d) { current() += d; }
    static void reset() { current() = time_point{duration{0}}; }

   private:
    static time_point& current() {
        static time_point t{duration{0}};
        return t;
    }
};

using ManualTtlStore = TtlCache<uint64_t, CacheEntry, ManualClock>;

// ================================================================ 构造与生命周期

TEST(SemanticCacheTest, NullEmbedderThrows) {
    std::shared_ptr<louis::cache::Embedder> noEmbedder;
    EXPECT_THROW(
        SemanticCache(Options{}, noEmbedder, std::make_unique<BruteForceIndex>(kDim)),
        std::invalid_argument
    );
}

TEST(SemanticCacheTest, NullVectorIndexThrows) {
    std::unique_ptr<VectorIndex> noIndex;
    EXPECT_THROW(
        SemanticCache(Options{}, std::make_shared<HashEmbedder>(), std::move(noIndex)),
        std::invalid_argument
    );
}

TEST(SemanticCacheTest, NullEntryStoreThrows) {
    std::unique_ptr<Policy<uint64_t, CacheEntry>> noStore;
    EXPECT_THROW(
        SemanticCache(
            Options{}, std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim),
            std::move(noStore)
        ),
        std::invalid_argument
    );
}

TEST(SemanticCacheTest, EmbedderAndIndexDimMismatchThrows) {
    EXPECT_THROW(
        SemanticCache(Options{}, std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(4)),
        std::invalid_argument
    );
}

// 回调捕获 this，拷贝/移动必然悬垂
TEST(SemanticCacheTest, IsNeitherCopyableNorMovable) {
    static_assert(!std::is_copy_constructible<SemanticCache>::value, "SemanticCache 不应可拷贝");
    static_assert(!std::is_copy_assignable<SemanticCache>::value, "SemanticCache 不应可拷贝赋值");
    static_assert(!std::is_move_constructible<SemanticCache>::value, "SemanticCache 不应可移动");
    static_assert(!std::is_move_assignable<SemanticCache>::value, "SemanticCache 不应可移动赋值");
}

// 便捷构造必须把 options.capacity 传递给默认 LruCache
TEST(SemanticCacheTest, ConvenienceCtorUsesOptionsCapacity) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(2), std::make_shared<HashEmbedder>(), std::move(index));

    cache.put(kWeather, "晴");
    cache.put(kParis, "餐厅");
    cache.put(kUnrelated, "模板");

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(indexRaw->size(), 2u);
    EXPECT_EQ(cache.stats().evictions, 1u);
}

// ================================================================ 精确路径（正确性）

TEST(SemanticCacheTest, ExactHitSkipsEmbeddingAndVectorSearch) {
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4), embedder, std::move(index));

    cache.put(kWeather, "晴");
    const int embedsAfterPut = embedder->calls.load();
    ASSERT_EQ(embedsAfterPut, 1);

    const GetResult r = cache.get(kWeather);

    EXPECT_TRUE(r.hit);
    EXPECT_TRUE(r.exact);
    EXPECT_EQ(r.response, "晴");
    EXPECT_FLOAT_EQ(r.score, 1.0f);
    EXPECT_EQ(embedder->calls.load(), embedsAfterPut) << "精确命中不得调用 embed";
    EXPECT_EQ(indexRaw->searches.load(), 0) << "精确命中不得访问向量索引";

    const auto st = cache.stats();
    EXPECT_EQ(st.exactHits, 1u);
    EXPECT_EQ(st.semanticHits, 0u);
    EXPECT_EQ(st.misses, 0u);
}

TEST(SemanticCacheTest, EmptyCacheMissesAndEmbedsOnce) {
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4), embedder, std::move(index));

    const GetResult r = cache.get(kWeather);

    EXPECT_FALSE(r.hit);
    EXPECT_EQ(embedder->calls.load(), 1);
    EXPECT_EQ(indexRaw->searches.load(), 1);
    EXPECT_EQ(cache.stats().misses, 1u);
}

// ================================================================ 语义路径（正确性 / 边界）

TEST(SemanticCacheTest, SemanticHitReturnsResponseAndScore) {
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    SemanticCache cache(makeOptions(4), embedder, std::move(index));

    cache.put(kWeather, "晴");
    const int embedsAfterPut = embedder->calls.load();

    const GetResult r = cache.get(kWeatherVariant);

    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.exact);
    EXPECT_EQ(r.response, "晴");
    EXPECT_GE(r.score, 0.85f);
    EXPECT_LT(r.score, 1.0f);
    EXPECT_EQ(embedder->calls.load(), embedsAfterPut + 1);

    const auto st = cache.stats();
    EXPECT_EQ(st.exactHits, 0u);
    EXPECT_EQ(st.semanticHits, 1u);
    EXPECT_EQ(st.misses, 0u);
}

TEST(SemanticCacheTest, UnrelatedQueryMisses) {
    SemanticCache cache(makeOptions(4), std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim));
    cache.put(kWeather, "晴");

    const GetResult r = cache.get(kUnrelated);

    EXPECT_FALSE(r.hit);
    EXPECT_LT(r.score, 0.85f);
    EXPECT_EQ(cache.stats().misses, 1u);
    EXPECT_EQ(cache.stats().semanticHits, 0u);
}

// 阈值比较是 >=：score 恰好等于阈值必须命中
TEST(SemanticCacheTest, ScoreEqualToThresholdHits) {
    auto index = std::make_unique<BruteForceIndex>(4);
    SemanticCache cache(makeOptions(4, /*threshold=*/1.0f), std::make_shared<ConstantEmbedder>(), std::move(index));
    cache.put("a", "ra");

    const GetResult r = cache.get("b");  // 精确未命中，语义分数恒为 1.0

    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.exact);
    EXPECT_EQ(r.response, "ra");
    EXPECT_FLOAT_EQ(r.score, 1.0f);
}

TEST(SemanticCacheTest, ScoreBelowThresholdMisses) {
    auto index = std::make_unique<BruteForceIndex>(4);
    SemanticCache cache(
        makeOptions(4, /*threshold=*/1.0001f), std::make_shared<ConstantEmbedder>(), std::move(index)
    );
    cache.put("a", "ra");

    const GetResult r = cache.get("b");

    EXPECT_FALSE(r.hit);
    EXPECT_EQ(cache.stats().misses, 1u);
}

// searchK == 0 时索引返回空，必须落 miss 而不是越界或命中
TEST(SemanticCacheTest, SearchKZeroMisses) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4, 0.85f, /*searchK=*/0), std::make_shared<HashEmbedder>(), std::move(index));
    cache.put(kWeather, "晴");

    const GetResult r = cache.get(kWeatherVariant);

    EXPECT_FALSE(r.hit);
    EXPECT_EQ(cache.stats().misses, 1u);
    EXPECT_EQ(indexRaw->size(), 1u) << "miss 不应影响索引内容";
}

// 统计口径：一次精确、一次语义、一次 miss 各计一次
TEST(SemanticCacheTest, StatsSplitAcrossPaths) {
    SemanticCache cache(makeOptions(8), std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim));
    cache.put(kWeather, "晴");
    cache.put(kParis, "餐厅");

    (void)cache.get(kWeather);        // 精确
    (void)cache.get(kWeatherVariant); // 语义
    (void)cache.get(kUnrelated);      // miss

    const auto st = cache.stats();
    EXPECT_EQ(st.puts, 2u);
    EXPECT_EQ(st.exactHits, 1u);
    EXPECT_EQ(st.semanticHits, 1u);
    EXPECT_EQ(st.misses, 1u);
}

// ================================================================ 容量 / 淘汰（生命周期）

TEST(SemanticCacheTest, CapacityOneEvictsPreviousEntry) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(1), std::make_shared<HashEmbedder>(), std::move(index));

    cache.put(kWeather, "晴");
    cache.put(kParis, "餐厅");

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(indexRaw->size(), 1u);
    EXPECT_EQ(cache.stats().evictions, 1u);

    EXPECT_FALSE(cache.get(kWeather).hit) << "被淘汰条目不得再命中";
    const GetResult kept = cache.get(kParis);
    EXPECT_TRUE(kept.hit);
    EXPECT_TRUE(kept.exact);
}

// 容量 0：put 必须早退，既不驻留也不 embed
TEST(SemanticCacheTest, CapacityZeroStoresNothingAndSkipsEmbeddingOnPut) {
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(0), embedder, std::move(index));

    cache.put(kWeather, "晴");

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(indexRaw->size(), 0u);
    EXPECT_EQ(embedder->calls.load(), 0) << "未驻留的 put 不应付出向量化成本";
    EXPECT_EQ(cache.stats().puts, 1u);

    EXPECT_FALSE(cache.get(kWeather).hit);
    EXPECT_EQ(embedder->calls.load(), 1);
}

// 精确命中会刷新 recency：A 被访问后，淘汰应落在 B 上
TEST(SemanticCacheTest, ExactHitRefreshesRecency) {
    SemanticCache cache(makeOptions(2), std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim));
    cache.put(kWeather, "晴");
    cache.put(kParis, "餐厅");
    ASSERT_TRUE(cache.get(kWeather).exact);

    cache.put(kUnrelated, "模板");

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_FALSE(cache.get(kParis).hit) << "LRU 应淘汰未被访问的 B";
    EXPECT_TRUE(cache.get(kWeather).exact);
    EXPECT_TRUE(cache.get(kUnrelated).exact);
}

// ================================================================ 删除（生命周期）

TEST(SemanticCacheTest, RemoveErasesVectorAndExactMapping) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4), std::make_shared<HashEmbedder>(), std::move(index));
    cache.put(kWeather, "晴");

    cache.remove(kWeather);

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(indexRaw->size(), 0u);
    EXPECT_EQ(cache.stats().evictions, 0u) << "显式删除不应计入 evictions";
    EXPECT_EQ(cache.stats().expirations, 0u);

    EXPECT_FALSE(cache.get(kWeather).hit) << "删除后不得幽灵命中";
}

TEST(SemanticCacheTest, RemoveUnknownOrRepeatedPromptIsNoop) {
    SemanticCache cache(makeOptions(4), std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim));
    EXPECT_NO_THROW(cache.remove("不存在的 prompt"));

    cache.put(kWeather, "晴");
    cache.remove(kWeather);
    EXPECT_NO_THROW(cache.remove(kWeather));
    EXPECT_EQ(cache.size(), 0u);
}

// ================================================================ 重复 put（边界 / 幂等性）

// 重复 put 必须替换旧条目：旧向量与精确映射一并离场，不占两份容量
TEST(SemanticCacheTest, DuplicatePutReplacesOldEntry) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(2), std::make_shared<HashEmbedder>(), std::move(index));

    cache.put(kWeather, "旧的");
    cache.put(kWeather, "新的");

    EXPECT_EQ(cache.size(), 1u) << "重复 put 应替换旧条目而不是并存";
    EXPECT_EQ(indexRaw->size(), 1u) << "旧向量必须随旧条目一起删除";
    EXPECT_EQ(cache.stats().evictions, 0u) << "替换属于显式删除，不应计入 evictions";

    const GetResult r = cache.get(kWeather);
    EXPECT_TRUE(r.hit);
    EXPECT_TRUE(r.exact);
    EXPECT_EQ(r.response, "新的");

    // 只占一格容量：再插一条不会触发淘汰
    cache.put(kParis, "餐厅");
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.stats().evictions, 0u);
    EXPECT_EQ(cache.get(kWeather).response, "新的");
    EXPECT_TRUE(cache.get(kParis).exact);
}

// 重复 put 替换后只剩最新条目：语义路径也必须返回最新响应
TEST(SemanticCacheTest, DuplicatePutSemanticHitReturnsLatestResponse) {
    SemanticCache cache(makeOptions(2), std::make_shared<HashEmbedder>(), std::make_unique<BruteForceIndex>(kDim));
    cache.put(kWeather, "旧的");
    cache.put(kWeather, "新的");

    const GetResult r = cache.get(kWeatherVariant);

    EXPECT_TRUE(r.hit);
    EXPECT_EQ(r.response, "新的") << "替换旧条目后不应再存在并列分的旧向量";
}

// ================================================================ TTL（生命周期 / 时序）

TEST(SemanticCacheTest, TtlExpiryOnGetRemovesVectorAndExactMapping) {
    ManualClock::reset();
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    auto inner = std::make_shared<LruCache<uint64_t, CacheEntry>>(8);
    auto store = std::make_unique<ManualTtlStore>(inner, ManualClock::duration{10'000});
    SemanticCache cache(makeOptions(8), embedder, std::move(index), std::move(store));

    cache.put(kWeather, "晴");

    ManualClock::advance(ManualClock::duration{11'000});
    const GetResult r = cache.get(kWeather);

    EXPECT_FALSE(r.hit);
    EXPECT_EQ(cache.stats().expirations, 1u);
    EXPECT_EQ(indexRaw->size(), 0u);
    EXPECT_EQ(cache.size(), 0u);

    EXPECT_FALSE(cache.get(kWeather).hit) << "过期条目不得幽灵命中";
}

TEST(SemanticCacheTest, TtlNotExpiredKeepsExactHit) {
    ManualClock::reset();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto inner = std::make_shared<LruCache<uint64_t, CacheEntry>>(8);
    auto store = std::make_unique<ManualTtlStore>(inner, ManualClock::duration{10'000});
    SemanticCache cache(makeOptions(8), std::make_shared<HashEmbedder>(), std::move(index), std::move(store));

    cache.put(kWeather, "晴");
    ManualClock::advance(ManualClock::duration{9'000});

    const GetResult r = cache.get(kWeather);

    EXPECT_TRUE(r.hit);
    EXPECT_TRUE(r.exact);
    EXPECT_EQ(cache.stats().expirations, 0u);
}

TEST(SemanticCacheTest, TtlPurgeViaSizeCountsExpiration) {
    ManualClock::reset();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    auto inner = std::make_shared<LruCache<uint64_t, CacheEntry>>(8);
    auto store = std::make_unique<ManualTtlStore>(inner, ManualClock::duration{10'000});
    SemanticCache cache(makeOptions(8), std::make_shared<HashEmbedder>(), std::move(index), std::move(store));

    cache.put(kWeather, "晴");
    ManualClock::advance(ManualClock::duration{11'000});

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(indexRaw->size(), 0u);
    EXPECT_EQ(cache.stats().expirations, 1u);
}

// ================================================================ 时序 / 竞态（确定性注入）

// put 的窗口：store.put 与 exists() 之后、vectorIndex.add() 之前被挤出。
// 缺少写后复核时，add 会把已离场条目的向量永久留在索引里。
TEST(SemanticCacheTest, EvictionBetweenExistsAndAddLeavesOrphanVector) {
    auto embedder = std::make_shared<HookEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    auto store = std::make_unique<LruCache<uint64_t, CacheEntry>>(1);
    auto* storeRaw = store.get();

    embedder->setHook([storeRaw] { storeRaw->put(9999, CacheEntry{"其它", "其它"}); });
    SemanticCache cache(makeOptions(1), embedder, std::move(index), std::move(store));

    cache.put(kWeather, "晴");

    ASSERT_EQ(cache.size(), 1u);
    EXPECT_EQ(indexRaw->size(), 0u) << "被驱逐条目的向量成了孤儿（put 缺少写后复核）";

    cache.remove(kWeather);
    EXPECT_EQ(indexRaw->size(), 0u) << "孤儿向量无法被 remove 清理（精确映射指向已离场 id）";
}

// put 先落 store、后算向量：embed 抛异常时条目已驻留但无向量、无精确映射
TEST(SemanticCacheTest, EmbedFailureMustNotLeaveResidentUnindexedEntry) {
    auto embedder = std::make_shared<ThrowingEmbedder>(2);
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    auto store = std::make_unique<LruCache<uint64_t, CacheEntry>>(8);
    auto* storeRaw = store.get();
    SemanticCache cache(makeOptions(8), embedder, std::move(index), std::move(store));

    cache.put(kWeather, "晴");
    ASSERT_EQ(cache.size(), 1u);

    EXPECT_THROW(cache.put(kParis, "餐厅"), std::runtime_error);

    EXPECT_EQ(cache.size(), 1u) << "失败的 put 不得留下未索引的条目";
    EXPECT_FALSE(storeRaw->exists(1u)) << "id=1 的条目应随 put 失败一并回滚";
    EXPECT_EQ(indexRaw->size(), cache.size());
}

// exactIndexEnabled == false 时 get 不得走精确路径
TEST(SemanticCacheTest, ExactIndexDisabledSkipsExactPath) {
    auto embedder = std::make_shared<CountingEmbedder>();
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4, 0.85f, 5, /*exact=*/false), embedder, std::move(index));

    cache.put(kWeather, "晴");
    const int embedsAfterPut = embedder->calls.load();
    const GetResult r = cache.get(kWeather);

    EXPECT_FALSE(r.exact) << "exactIndexEnabled=false 时仍走了精确路径";
    EXPECT_EQ(embedder->calls.load(), embedsAfterPut + 1);
    EXPECT_EQ(indexRaw->searches.load(), 1);
    EXPECT_EQ(cache.stats().exactHits, 0u);
    EXPECT_EQ(cache.stats().semanticHits, 1u);
}

// ================================================================ 并发（线程安全）

// 混合 put / get / remove：结束后索引与存储必须同量
TEST(SemanticCacheTest, ConcurrentMixedOperationsKeepSizesInSync) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(64), std::make_shared<HashEmbedder>(), std::move(index));

    constexpr int kThreads = 8;
    constexpr int kOps = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < kOps; ++i) {
                const std::string prompt = "并发-" + std::to_string(t) + "-" + std::to_string(i);
                cache.put(prompt, "r");
                (void)cache.get(prompt);
                if (i % 3 == 0) cache.remove(prompt);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(cache.size(), indexRaw->size());
    EXPECT_EQ(cache.stats().puts, static_cast<uint64_t>(kThreads) * kOps);
}

// 小容量 + 高频写入：淘汰与 put 交错，最易暴露「写后无复核」造成的孤儿向量
TEST(SemanticCacheTest, ConcurrentEvictionKeepsIndexAndStoreInSync) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(4), std::make_shared<HashEmbedder>(), std::move(index));

    constexpr int kThreads = 8;
    constexpr int kOps = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < kOps; ++i) {
                const std::string prompt = "淘汰-" + std::to_string(t) + "-" + std::to_string(i);
                cache.put(prompt, "r");
                if (i % 7 == 0) (void)cache.get(prompt);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(cache.size(), 4u);
    EXPECT_EQ(indexRaw->size(), cache.size()) << "索引与存储脱节：存在孤儿向量或漏索引条目";
}

// 同一 prompt 并发 put + get：exactIndex_ 的写路径也必须被保护
TEST(SemanticCacheTest, ConcurrentSamePromptPutAndGet) {
    auto index = std::make_unique<CountingIndex>(kDim);
    auto* indexRaw = index.get();
    SemanticCache cache(makeOptions(1024), std::make_shared<HashEmbedder>(), std::move(index));

    constexpr int kThreads = 8;
    constexpr int kOps = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < kOps; ++i) {
                cache.put(kWeather, "r" + std::to_string(t));
                const GetResult r = cache.get(kWeather);
                if (r.hit) EXPECT_FALSE(r.response.empty());
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(cache.size(), indexRaw->size());
}

}  // namespace
