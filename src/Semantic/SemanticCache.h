#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "CacheStats.h"
#include "Embedder.h"
#include "LRU/LruCache.h"
#include "Options.h"
#include "Policy.h"
#include "VectorIndex.h"

namespace louis::cache {
struct GetResult {
    bool hit = false;
    std::string response{};
    float score = 0.0f;  // 精确命中记 1.0f
    bool exact = false;  // 是否由精确匹配命中
};

// 条目值类型：必须携带 prompt，onLeave 才能反查（并擦除）exactIndex_ 映射。
struct CacheEntry {
    std::string prompt;
    std::string response;
};

class SemanticCache {
   public:
    // 主构造：三个运行时依赖全部由调用方注册
    // 契约：store 必须是本缓存独占、新建且未设置过回调的实例
    SemanticCache(
        Options options, std::shared_ptr<Embedder> embedder, std::unique_ptr<VectorIndex> vectorIndex,
        std::unique_ptr<Policy<uint64_t, CacheEntry>> store
    );
    // 便捷构造：按 options.capacity 新建 LruCache， 转调主构造
    SemanticCache(
        Options options, std::shared_ptr<Embedder> embedder, std::unique_ptr<VectorIndex> vectorIndex
    );
    // store_ 的回调捕获 this, 禁止拷贝/移动
    SemanticCache(const SemanticCache&) = delete;
    SemanticCache& operator=(const SemanticCache&) = delete;
    SemanticCache(SemanticCache&&) = delete;
    SemanticCache& operator=(SemanticCache&&) = delete;

    GetResult get(const std::string& prompt);
    void put(const std::string& prompt, const std::string& response);
    void remove(const std::string& prompt);

    CacheStats stats() const;  // 状态快照
    size_t size() const;       // = store_->size()

   private:
    // 唯一清理出口，由 store_ 在锁外、本线程调用栈内触发
    void onLeave(uint64_t id, const CacheEntry& entry, LeaveReason reason);

    std::optional<uint64_t> findExactLocked(const std::string& prompt) const;
    void eraseExactIfMatchesLocked(const std::string& prompt, uint64_t id);

    static int clampCapacity(size_t capacity);  // LruCache 的容量是 int

    Options options_;
    std::shared_ptr<const Embedder> embedder_;
    std::unique_ptr<VectorIndex> vectorIndex_;
    std::unique_ptr<Policy<uint64_t, CacheEntry>> store_;

    mutable std::mutex mutex_;                              // 保护下方数据
    std::unordered_map<std::string, uint64_t> exactIndex_;  // 嵌入式精确索引
    std::atomic<uint64_t> nextId_{0};                       // 下一个可用 ID
    CacheStats stats_;
};

inline int SemanticCache::clampCapacity(size_t capacity) {
    constexpr auto kIntMax = static_cast<size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(capacity > kIntMax ? kIntMax : capacity);
}

inline SemanticCache::SemanticCache(
    Options options, std::shared_ptr<Embedder> embedder, std::unique_ptr<VectorIndex> vectorIndex
)
    : SemanticCache(
          options, embedder, std::move(vectorIndex),
          std::make_unique<LruCache<uint64_t, CacheEntry>>(clampCapacity(options.capacity))
      ) {}

inline SemanticCache::SemanticCache(
    Options options, std::shared_ptr<Embedder> embedder, std::unique_ptr<VectorIndex> vectorIndex,
    std::unique_ptr<Policy<uint64_t, CacheEntry>> store
)
    : options_(options),
      embedder_(std::move(embedder)),
      vectorIndex_(std::move(vectorIndex)),
      store_(std::move(store)) {
    if (!embedder_) throw std::invalid_argument("SemanticCache: embedder_ is null");
    if (!vectorIndex_) throw std::invalid_argument("SemanticCache: vectorIndex_ is null");
    if (!store_) throw std::invalid_argument("SemanticCache: store_ is null");

    if (embedder_->dim() != vectorIndex_->dim())
        throw std::invalid_argument("SemanticCache: embedder_ and vectorIndex_ dim must be the same");

    store_->setLeaveCallback([this](const uint64_t& id, const CacheEntry& entry, LeaveReason reason) {
        onLeave(id, entry, reason);
    });
}

inline GetResult SemanticCache::get(const std::string& prompt) {
    // 先查 exactIndex_
    std::optional<uint64_t> id{};
    CacheEntry entry{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = findExactLocked(prompt);
    }
    if (id && store_->get(*id, entry)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.exactHits++;
        }
        return {true, entry.response, 1.0f, true};
    }

    // 没有再查 VectorIndex
    auto matches = vectorIndex_->search(embedder_->embed(prompt), options_.searchK);
    if (matches.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.misses++;
        }
        return {};
    };
    VectorMatch bestMatch = matches.front();
    if (bestMatch.score < options_.hitThreshold) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.misses++;
        }
        return {false, "", bestMatch.score, false};
    }

    // 最后 通过 bestMatch 找到最相似的条目
    if (store_->get(bestMatch.id, entry)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.semanticHits++;
        }
        return {true, entry.response, bestMatch.score, false};
    }

    // 没找到返回 miss
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.misses++;
    }
    return {};
}

inline void SemanticCache::put(const std::string& prompt, const std::string& response) {
    auto id = nextId_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.puts++;
    }

    // 先写入，如果写入失败直接返回
    store_->put(id, {prompt, response});
    if (!store_->exists(id)) return;

    // 再索引
    vectorIndex_->add(id, embedder_->embed(prompt));
    // 最后精确索引
    exactIndex_[prompt] = id;
}

inline void SemanticCache::remove(const std::string& prompt) {
    auto it = exactIndex_.end();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        it = exactIndex_.find(prompt);
    }
    if (it == exactIndex_.end()) return;
    store_->remove(it->second);  // 回调 onLeave(Explicit) 统一删向量 + 精确映射
}

inline void SemanticCache::onLeave(uint64_t id, const CacheEntry& entry, LeaveReason reason) {
    // 先删 VectorIndex
    vectorIndex_->remove(id);
    // 再删 exactIndex_：仅当映射仍指向本条目，避免误删同 prompt 的新条目
    std::lock_guard<std::mutex> lock(mutex_);
    eraseExactIfMatchesLocked(entry.prompt, id);
    if (reason == LeaveReason::Evicted) {
        stats_.evictions++;
    } else if (reason == LeaveReason::Expired) {
        stats_.expirations++;
    }
}

inline std::optional<uint64_t> SemanticCache::findExactLocked(const std::string& prompt) const {
    auto it = exactIndex_.find(prompt);
    if (it == exactIndex_.end()) return std::nullopt;
    return it->second;
}

inline void SemanticCache::eraseExactIfMatchesLocked(const std::string& prompt, uint64_t id) {
    auto it = exactIndex_.find(prompt);
    if (it != exactIndex_.end() && it->second == id) exactIndex_.erase(it);
}
}  // namespace louis::cache
