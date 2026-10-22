#include "Semantic/VectorIndex.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Semantic/Embedder.h"

// 本文件以「契约驱动」方式覆盖 VectorIndex / BruteForceIndex 的边界。
//
// 声明的契约（实现需保证）：
//  1. 维度在构造期固定（VectorIndex(dim)），dim == 0 构造即抛 std::invalid_argument。
//  2. search 返回条数 <= min(k, size())，且只能包含真实驻留的 id；不足 k 条时不得补位。
//  3. 空索引 / k == 0 返回空结果。
//  4. add 的向量维度必须等于 dim()，空向量同样拒绝：抛 std::invalid_argument，且不得污染索引。
//  5. search 的 query 维度与 dim() 不一致（含空 query）时返回空（读路径不抛异常，落到 miss 安全侧）。
//  6. 分数并列时次序必须确定（按 id 升序），保证结果可复现。
//  7. remove 幂等；重复 add 同一 id 视为覆盖。
//  8. 所有方法线程安全，size() 与 add/remove 的净数量一致。

namespace {

using louis::cache::BruteForceIndex;
using louis::cache::HashEmbedder;

// 归一化基向量：只有第 i 个分量为 1。任意两个不同基向量正交（余弦 = 0），
// 便于精确构造期望分数，不依赖 HashEmbedder 的哈希分布。
std::vector<float> basis(size_t dim, size_t i) {
    std::vector<float> v(dim, 0.0f);
    v[i] = 1.0f;
    return v;
}

// 与 Options::hitThreshold 默认值保持一致
constexpr float kHitThreshold = 0.85f;

// ---------------------------------------------------------------- 构造期维度

// 维度是构造期不变式，对外可读
TEST(VectorIndexTest, DimIsFixedAtConstruction) {
    BruteForceIndex index(4);

    EXPECT_EQ(index.dim(), 4u);
}

// 维度 0 无意义：构造即失败，避免后续 add/search 出现零长向量语义
TEST(VectorIndexTest, ZeroDimThrows) { EXPECT_THROW(BruteForceIndex(0), std::invalid_argument); }

// ---------------------------------------------------------------- search 基本行为

// 空索引：无论 k 多大都应返回空，不能凭空补出 k 条 {id=0, score=0}
TEST(VectorIndexTest, EmptyIndexSearchReturnsEmpty) {
    BruteForceIndex index(3);
    const auto matches = index.search(basis(3, 0), 5);

    EXPECT_TRUE(matches.empty());
}

// 空索引 + 维度不符的 query：同样返回空（维度校验先于"是否为空"判断）
TEST(VectorIndexTest, EmptyIndexMismatchedQueryReturnsEmpty) {
    BruteForceIndex index(3);

    EXPECT_TRUE(index.search(basis(4, 0), 5).empty());
}

// 空索引 + k == 0：返回空
TEST(VectorIndexTest, ZeroKReturnsEmpty) {
    BruteForceIndex index(3);
    EXPECT_TRUE(index.search(basis(3, 0), 0).empty());

    index.add(1, basis(3, 0));
    EXPECT_TRUE(index.search(basis(3, 0), 0).empty());
}

// 命中自身：归一化向量与自身点积为 1
TEST(VectorIndexTest, SelfSearchScoreIsOne) {
    BruteForceIndex index(4);
    index.add(7, basis(4, 2));

    const auto matches = index.search(basis(4, 2), 1);

    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].id, 7u);
    EXPECT_NEAR(matches[0].score, 1.0f, 1e-6f);
}

// top-k 按分数降序
TEST(VectorIndexTest, TopKIsDescendingByScore) {
    BruteForceIndex index(4);
    index.add(1, basis(4, 0));
    index.add(2, basis(4, 1));
    index.add(3, basis(4, 2));
    // 与 id1 余弦 0.6、id2 余弦 0.8、id3 余弦 0.0
    const std::vector<float> query = {0.6f, 0.8f, 0.0f, 0.0f};

    const auto matches = index.search(query, 2);

    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].id, 2u);
    EXPECT_NEAR(matches[0].score, 0.8f, 1e-6f);
    EXPECT_EQ(matches[1].id, 1u);
    EXPECT_NEAR(matches[1].score, 0.6f, 1e-6f);
}

// k 恰好等于条目数：全部返回
TEST(VectorIndexTest, KEqualsSizeReturnsAll) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));
    index.add(2, basis(3, 1));

    const auto matches = index.search(basis(3, 0), 2);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].id, 1u);
}

// k > 条目数：只能返回真实条目，不能补位
TEST(VectorIndexTest, KGreaterThanSizeReturnsOnlyExisting) {
    BruteForceIndex index(3);
    index.add(100, basis(3, 0));
    index.add(200, basis(3, 1));

    const auto matches = index.search(basis(3, 0), 5);

    ASSERT_EQ(matches.size(), 2u) << "不足 k 条时不得补位";
    for (const auto& m : matches) {
        EXPECT_TRUE(m.id == 100 || m.id == 200) << "出现幽灵 id=" << m.id;
    }
}

// k 极大时同样不能补位（同时避免按 k 盲目分配内存）
TEST(VectorIndexTest, HugeKDoesNotPad) {
    BruteForceIndex index(2);
    index.add(1, basis(2, 0));

    const auto matches = index.search(basis(2, 0), 1'000'000);

    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].id, 1u);
}

// 并列分：次序必须确定（按 id 升序），否则每次运行结果可能不同
TEST(VectorIndexTest, TieScoresAreDeterministicById) {
    BruteForceIndex index(2);
    const std::vector<uint64_t> ids = {42, 7, 19, 3, 8, 11, 5, 2, 23, 1};
    for (uint64_t id : ids) index.add(id, basis(2, 0));  // 向量完全相同 → 分数全等

    const auto matches = index.search(basis(2, 0), ids.size());

    ASSERT_EQ(matches.size(), ids.size());
    for (size_t i = 1; i < matches.size(); ++i) {
        EXPECT_LT(matches[i - 1].id, matches[i].id)
            << "并列分次序不确定：第 " << i << " 条 id=" << matches[i].id;
    }
}

// ---------------------------------------------------------------- remove / add 语义

// remove 后：搜索结果与 size 都不应再包含该 id（无幽灵命中）
TEST(VectorIndexTest, RemoveMakesEntryInvisible) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));
    index.add(2, basis(3, 0));

    index.remove(1);

    EXPECT_EQ(index.size(), 1u);
    const auto matches = index.search(basis(3, 0), 5);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].id, 2u);
}

// remove 不存在的 id：幂等空操作，不抛、不改 size
TEST(VectorIndexTest, RemoveNonexistentIsNoop) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));

    EXPECT_NO_THROW(index.remove(999));
    EXPECT_EQ(index.size(), 1u);
}

// 重复 add 同一 id：覆盖旧向量，size 不增长
TEST(VectorIndexTest, AddSameIdOverwrites) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));
    index.add(1, basis(3, 1));

    EXPECT_EQ(index.size(), 1u);

    const auto matches = index.search(basis(3, 1), 1);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].id, 1u);
    EXPECT_NEAR(matches[0].score, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------- 维度校验

// 维度不一致的 add 属于调用方错误：抛 std::invalid_argument，且不得污染索引
TEST(VectorIndexTest, AddInconsistentDimThrows) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));

    EXPECT_THROW(index.add(2, basis(4, 1)), std::invalid_argument);
    EXPECT_EQ(index.size(), 1u);
}

// 空向量维度为 0，与 dim() 不符：抛 std::invalid_argument，且不得污染索引
TEST(VectorIndexTest, AddEmptyVectorThrows) {
    BruteForceIndex index(3);

    EXPECT_THROW(index.add(1, {}), std::invalid_argument);
    EXPECT_EQ(index.size(), 0u);

    // 空向量被拒后，索引仍应接受正常维度
    index.add(2, basis(3, 0));
    EXPECT_EQ(index.size(), 1u);
}

// query 比索引维度短：应判定为维度不符，返回空，而不是算"部分点积"当命中
TEST(VectorIndexTest, SearchWithShorterQueryReturnsEmpty) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));

    const auto matches = index.search(std::vector<float>{1.0f, 0.0f}, 5);

    EXPECT_TRUE(matches.empty()) << "维度不符不得返回部分点积作为相似度";
}

// query 比索引维度长：不得越界读取已存向量，应按维度不符返回空。
// 放在文件末尾，避免万一崩溃影响前面用例的执行结果。
TEST(VectorIndexTest, SearchWithLongerQueryReturnsEmpty) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));

    const auto matches = index.search(std::vector<float>{1.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 5);

    EXPECT_TRUE(matches.empty()) << "维度不符不得越界读取已存向量";
}

// query 为空：等同于维度不符，返回空
TEST(VectorIndexTest, SearchWithEmptyQueryReturnsEmpty) {
    BruteForceIndex index(3);
    index.add(1, basis(3, 0));
    index.add(2, basis(3, 1));

    EXPECT_TRUE(index.search({}, 5).empty());
}

// ---------------------------------------------------------------- 计数与并发

// size 始终等于 add/remove 的净数量
TEST(VectorIndexTest, SizeTracksAddRemove) {
    BruteForceIndex index(2);
    EXPECT_EQ(index.size(), 0u);

    index.add(1, basis(2, 0));
    index.add(2, basis(2, 1));
    EXPECT_EQ(index.size(), 2u);

    index.remove(1);
    EXPECT_EQ(index.size(), 1u);

    index.remove(1);  // 幂等
    EXPECT_EQ(index.size(), 1u);

    index.add(1, basis(2, 0));  // 重新加入
    EXPECT_EQ(index.size(), 2u);
}

// 并发 add / remove / search / size：不崩、最终计数正确
TEST(VectorIndexTest, ConcurrentAddRemoveSearch) {
    BruteForceIndex index(4);
    constexpr int kThreads = 8;
    constexpr int kOps = 200;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&index, t] {
            for (int i = 0; i < kOps; ++i) {
                const uint64_t id = static_cast<uint64_t>(t) * kOps + static_cast<uint64_t>(i);
                index.add(id, basis(4, id % 4));
                const auto matches = index.search(basis(4, id % 4), 3);
                for (const auto& m : matches) {
                    EXPECT_LT(m.id, static_cast<uint64_t>(kThreads) * kOps);
                }
                if (i % 2 == 0) index.remove(id);
                (void)index.size();
            }
        });
    }
    for (auto& t : threads) t.join();

    // 每个线程一半的 id 被移除
    EXPECT_EQ(index.size(), static_cast<size_t>(kThreads) * (kOps / 2));
}

// 端到端：HashEmbedder 产出的归一化向量经索引检索后，阈值判定口径仍然成立
TEST(VectorIndexTest, HashEmbedderEndToEndThreshold) {
    HashEmbedder embedder;
    BruteForceIndex index(embedder.dim());

    index.add(1, embedder.embed("今天天气怎么样"));
    index.add(2, embedder.embed("巴黎有哪些好吃的餐厅"));

    const auto near = index.search(embedder.embed("今天的天气怎么样"), 1);
    ASSERT_EQ(near.size(), 1u);
    EXPECT_EQ(near[0].id, 1u);
    EXPECT_GE(near[0].score, kHitThreshold);

    const auto unrelated = index.search(embedder.embed("如何学习 C++ 模板元编程"), 1);
    ASSERT_EQ(unrelated.size(), 1u);
    EXPECT_LT(unrelated[0].score, kHitThreshold);
}

}  // namespace
