#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "LRU/LruCache.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value, typename HashFunc = std::hash<Key>>
class ShardedLruCache : public Policy<Key, Value> {
   public:
    ShardedLruCache(size_t capacity, int sliceNum = std::thread::hardware_concurrency())
        : sliceNum_(sliceNum <= 0 ? 1 : sliceNum) {
        // 精确分摊：前 remainder 个分片各多分 1 个，各分片容量之和恰为 capacity
        size_t base = capacity / sliceNum_;
        size_t remainder = capacity % sliceNum_;
        for (size_t i = 0; i < static_cast<size_t>(sliceNum_); ++i) {
            size_t sliceSize = base + (i < remainder ? 1 : 0);
            lruSliceCaches_.emplace_back(std::make_unique<LruCache<Key, Value>>(sliceSize));
            lruSliceCaches_.back()->setLeaveCallback(
                [this](const Key& k, const Value& v, LeaveReason r) {
                    // 分片在自身锁外分发事件，本类无外层锁，直接转发即可
                    this->notifyLeave(k, v, r);
                }
            );
        }
    }

    // 禁止拷贝和移动
    // 分片回调捕获本对象 this，移动会使闭包内指针悬垂，故禁止拷贝/移动
    ShardedLruCache(const ShardedLruCache&) = delete;
    ShardedLruCache& operator=(const ShardedLruCache&) = delete;
    ShardedLruCache(ShardedLruCache&&) = delete;
    ShardedLruCache& operator=(ShardedLruCache&&) = delete;

    void put(Key key, Value value) override {
        size_t sliceIndex = hash(key);
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(const Key& key, Value& value) override {
        size_t sliceIndex = hash(key);
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    void remove(const Key& key) override {
        size_t sliceIndex = hash(key);
        lruSliceCaches_[sliceIndex]->remove(key);
    }

    size_t size() const override {
        size_t size = 0;
        for (const auto& cache : lruSliceCaches_) {
            size += cache->size();
        }
        return size;
    }

   private:
    size_t hash(const Key& key) const { return hashFunc_(key) % sliceNum_; }

    int sliceNum_;                                                       // 分片数量
    std::vector<std::unique_ptr<LruCache<Key, Value>>> lruSliceCaches_;  // 存储每一个LRU缓存分片
    HashFunc hashFunc_;                                                  // 哈希函数
};
}  // namespace louis::cache