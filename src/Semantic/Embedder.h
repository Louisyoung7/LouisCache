#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace louis::cache {

class Embedder {
   public:
    virtual ~Embedder() = default;
    virtual std::vector<float> embed(const std::string& text) const = 0;
    virtual size_t dim() const = 0;
    // 模型指纹：向量空间标识，持久化索引加载时校验，不同指纹不可比较
    virtual std::string modelId() const = 0;
};

// 确定性、可复现的假向量生成器：仅用于测试编排逻辑，不代表语义质量。
// 线程安全：无内部可变状态，embed() 为 const 纯函数。
class HashEmbedder : public Embedder {
   public:
    // dim/ngram 参与 modelId 指纹：改了它们就换了向量空间
    explicit HashEmbedder(size_t dim = 128, size_t ngram = 2)
        : dim_(dim == 0 ? 1 : dim), ngram_(ngram == 0 ? 1 : ngram) {}

    std::vector<float> embed(const std::string& text) const override;
    size_t dim() const override { return dim_; }
    std::string modelId() const override;  // "hash-embedder:v1:fnv1a64:ngram=2:dim=128"

   private:
    // 标准 FNV-1a 64 位偏移基数，作哈希初值；固定不变以保证跨进程可复现
    static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    static uint64_t fnv1a(const unsigned char* p, size_t n, uint64_t seed);
    static uint64_t mix(uint64_t h);  // 位白化，见 §3
    size_t dim_;
    size_t ngram_;
};

// FNV-1a 64 位：以 seed 为初值，逐字节先异或、再乘质数。
// 标准 FNV-1a 传偏移基数 0xcbf29ce484222325 作 seed；换 seed 即可隔离不同向量空间。
inline uint64_t HashEmbedder::fnv1a(const unsigned char* p, size_t n, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 0x100000001b3ULL;  // FNV-1a 64 位质数
    }
    return h;
}

// MurmurHash3 64 位终结器（fmix64）：位白化，改善低位分布，避免 % dim 取桶时偏斜。
// FNV-1a 本身低位规律性弱，直接取模会加剧碰撞，故取桶前先过一遍。
inline uint64_t HashEmbedder::mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// 特征哈希（hashing trick）：滑动 n-gram → 哈希 → 定桶 + 定符号 → 累加 → L2 归一化。
// 带符号累加使向量均值≈0，cosine 近似"共享 n-gram 占比"，无关文本相互抵消而非恒正。
inline std::vector<float> HashEmbedder::embed(const std::string& text) const {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const size_t len = text.size();
    std::vector<float> v(dim_, 0.0f);

    // 长度不足 ngram 时用整串作为唯一特征（含空串，len=0），保证至少产出一个非零分量
    const size_t window = len < ngram_ ? len : ngram_;
    const size_t count = len < ngram_ ? 1 : len - ngram_ + 1;

    for (size_t i = 0; i < count; ++i) {
        const uint64_t h = mix(fnv1a(p + i, window, kFnvOffsetBasis));
        const size_t bucket = static_cast<size_t>(h % dim_);
        v[bucket] += (h >> 63) != 0 ? -1.0f : 1.0f;
    }

    // L2 归一化：cosine 退化为点积，与文本长度解耦，与真实模型口径一致
    float sq = 0.0f;
    for (float x : v) sq += x * x;
    const float norm = std::sqrt(sq);
    if (norm < 1e-6f) {
        // 理论上累加恰好抵消为 0 的兜底：单桶重建，避免 cosine 出现 0/0 = NaN
        v.assign(dim_, 0.0f);
        v[static_cast<size_t>(mix(fnv1a(p, len, kFnvOffsetBasis)) % dim_)] = 1.0f;
        return v;
    }
    for (float& x : v) x /= norm;
    return v;
}

// 指纹把影响向量空间的所有参数编进来；任何改变向量空间语义的改动（算法/常量）都要 bump 版本号，
// 否则持久化索引加载时会误判为同一空间，算出的相似度全部无意义。
inline std::string HashEmbedder::modelId() const {
    return "hash-embedder:v1:fnv1a64:ngram=" + std::to_string(ngram_) + ":dim=" + std::to_string(dim_);
}

}  // namespace louis::cache