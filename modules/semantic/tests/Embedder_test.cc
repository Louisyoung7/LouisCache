#include "Semantic/Embedder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace {

using louis::cache::HashEmbedder;

// 向量已归一化，余弦相似度即点积（用 double 累加避免精度干扰）
double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i) dot += static_cast<double>(a[i]) * b[i];
    return dot;
}

double l2norm(const std::vector<float>& v) {
    double sq = 0.0;
    for (float x : v) sq += static_cast<double>(x) * x;
    return std::sqrt(sq);
}

// 与 Options::hitThreshold 的默认值保持一致
constexpr double kHitThreshold = 0.85;

// 同一文本必得逐字节相同的向量：跨实例、跨调用都稳定（可复现性的核心）
TEST(HashEmbedderTest, DeterministicSameInput) {
    HashEmbedder a;
    HashEmbedder b;
    const std::string text = "今天天气怎么样";

    EXPECT_EQ(a.embed(text), b.embed(text));
    EXPECT_EQ(a.embed(text), a.embed(text));
}

// 维度自洽，且输出为单位向量
TEST(HashEmbedderTest, DimAndUnitNorm) {
    HashEmbedder e;
    const auto v = e.embed("帮我总结一下这篇文章");

    EXPECT_EQ(v.size(), e.dim());
    EXPECT_NEAR(l2norm(v), 1.0, 1e-5);
}

// 空串 / 单字符 / 短于 ngram 的串：维度正确、非零、单位长度
TEST(HashEmbedderTest, EmptyAndShortInput) {
    HashEmbedder e;
    const std::string inputs[] = {"", "a", "中", "中a"};

    for (const std::string& s : inputs) {
        const auto v = e.embed(s);
        EXPECT_EQ(v.size(), e.dim()) << "input=" << s;
        EXPECT_NEAR(l2norm(v), 1.0, 1e-5) << "input=" << s;
    }

    // 空串与自身相似度为 1，且不产生 0/0 = NaN
    const auto empty = e.embed("");
    EXPECT_NEAR(cosine(empty, empty), 1.0, 1e-5);
    EXPECT_FALSE(std::isnan(cosine(empty, empty)));
}

// 轻微改写的文本相似度应达到命中阈值（短句，1 字之差）
TEST(HashEmbedderTest, SimilarTextHighSimilarity) {
    HashEmbedder e;
    const auto base = e.embed("今天天气怎么样");
    const auto near = e.embed("今天的天气怎么样");

    EXPECT_GE(cosine(base, near), kHitThreshold);
    EXPECT_NEAR(cosine(base, base), 1.0, 1e-5);
}

// 长文本改写同样命中（1 字差异占比更小，余量更足）
TEST(HashEmbedderTest, LongTextParaphraseHits) {
    HashEmbedder e;
    const auto a = e.embed("帮我总结一下这篇关于分布式缓存一致性的文章");
    const auto b = e.embed("帮我总结一下这篇关于分布式缓存一致性的文章吧");

    EXPECT_GE(cosine(a, b), kHitThreshold);
}

// 无关文本必须低于阈值，且与相关文本拉开序关系
TEST(HashEmbedderTest, UnrelatedTextLowSimilarity) {
    HashEmbedder e;
    const auto base = e.embed("今天天气怎么样");
    const auto near = e.embed("今天的天气怎么样");
    const auto unrelated = e.embed("巴黎有哪些好吃的餐厅");

    EXPECT_LT(cosine(base, unrelated), kHitThreshold);
    EXPECT_LT(cosine(base, unrelated), 0.3);
    EXPECT_GT(cosine(base, near), cosine(base, unrelated));
}

// 极小维度：所有不变量仍成立（覆盖累加抵消后的兜底重建路径）
TEST(HashEmbedderTest, TinyDimKeepsInvariants) {
    HashEmbedder e(1, 2);
    const std::string inputs[] = {"", "ab", "abcd", "今天天气怎么样"};

    for (const std::string& s : inputs) {
        const auto v = e.embed(s);
        EXPECT_EQ(v.size(), 1u) << "input=" << s;
        EXPECT_NEAR(l2norm(v), 1.0, 1e-5) << "input=" << s;
    }
}

// 指纹编码了全部影响向量空间的参数
TEST(HashEmbedderTest, ModelIdEncodesParams) {
    HashEmbedder a;         // dim=128, ngram=2
    HashEmbedder b;         // 同配置
    HashEmbedder c(64, 2);  // 换 dim
    HashEmbedder d(128, 3); // 换 ngram

    EXPECT_EQ(a.modelId(), b.modelId());
    EXPECT_NE(a.modelId(), c.modelId());
    EXPECT_NE(a.modelId(), d.modelId());
    EXPECT_EQ(a.modelId(), "hash-embedder:v1:fnv1a64:ngram=2:dim=128");
}

// 非法参数在构造期被夹到 1，不产生 % 0 或零长窗口
TEST(HashEmbedderTest, ZeroParamsAreClamped) {
    HashEmbedder e(0, 0);

    EXPECT_EQ(e.dim(), 1u);
    EXPECT_EQ(e.modelId(), "hash-embedder:v1:fnv1a64:ngram=1:dim=1");

    const auto v = e.embed("今天天气怎么样");
    EXPECT_EQ(v.size(), 1u);
    EXPECT_NEAR(l2norm(v), 1.0, 1e-5);
}

// 无内部可变状态：并发调用结果与单线程一致
TEST(HashEmbedderTest, ConcurrentEmbed) {
    HashEmbedder e;
    const std::string text = "今天天气怎么样";
    const auto expected = e.embed(text);

    std::vector<int> ok(8, 0);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < ok.size(); ++i) {
        threads.emplace_back([&e, &ok, &text, &expected, i] { ok[i] = e.embed(text) == expected ? 1 : 0; });
    }
    for (auto& t : threads) t.join();

    for (size_t i = 0; i < ok.size(); ++i) EXPECT_EQ(ok[i], 1) << "thread=" << i;
}

}  // namespace
