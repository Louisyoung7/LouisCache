#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace louis::cache {
// 一条向量匹配结果：ID 与相似度（越大越相似）
struct VectorMatch {
    uint64_t id;
    float score;
};

class VectorIndex {
   public:
    virtual ~VectorIndex() = default;
    // 添加向量
    virtual void add(uint64_t id, const std::vector<float>& vector) = 0;
    // 删除向量
    virtual void remove(uint64_t id) = 0;
    // 搜索向量,返回TopK个匹配结果
    virtual std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const = 0;
    // 获取向引中向量的数量，应和EntryStore中的实际缓存数量一致
    virtual size_t size() const = 0;
};

class BruteForceIndex : public VectorIndex {
   public:
    void add(uint64_t id, const std::vector<float>& vector) override;
    void remove(uint64_t id) override;
    std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const override;
    size_t size() const override;

   private:
    std::unordered_map<uint64_t, std::vector<float>> vectors_;
    mutable std::mutex mutex_;
};

inline void BruteForceIndex::add(uint64_t id, const std::vector<float>& vector) {
    std::lock_guard<std::mutex> lock(mutex_);
    vectors_[id] = vector;
}

inline void BruteForceIndex::remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    vectors_.erase(id);
}

inline std::vector<VectorMatch> BruteForceIndex::search(const std::vector<float>& query, size_t k) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VectorMatch> matches;
    // 全量扫描所有向量，计算相似度
    for (const auto& [id, vector] : vectors_) {
        float score = 0.0f;
        // 累加计算点积
        for (size_t i = 0; i < query.size(); ++i) {
            score += query[i] * vector[i];
        }
        matches.push_back({id, score});
    }
    // 按相似度从大到小排序
    std::sort(matches.begin(), matches.end(), [](const VectorMatch& a, const VectorMatch& b) {
        return a.score > b.score;
    });
    // 取TopK个匹配结果
    matches.resize(k);
    return matches;
}

inline size_t BruteForceIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vectors_.size();
}
}  // namespace louis::cache
