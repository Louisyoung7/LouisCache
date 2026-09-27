#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "LFU/LfuCache.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value, typename HashFunc = std::hash<Key>>
class ShardedLfuCache : public Policy<Key, Value> {
   public:
    ShardedLfuCache(size_t capacity, int sliceNum = std::thread::hardware_concurrency())
        : capacity_(capacity), sliceNum_(sliceNum <= 0 ? 1 : sliceNum) {
        // 分片数钳制：非正值归 1；不超过 capacity，避免容量 0 的死分片
        size_t num = sliceNum > 0 ? static_cast<size_t>(sliceNum) : 1;
        if (num > capacity) num = capacity > 0 ? capacity : 1;  // 分片数即 capacity
        sliceNum_ = static_cast<int>(num);

        // 精确分摊：前 remainder 个分片各多分 1 个，各分片容量之和恰为 capacity
        size_t base = capacity / sliceNum_;
        size_t remainder = capacity % sliceNum_;
        for (size_t i = 0; i < static_cast<size_t>(sliceNum_); ++i) {
            size_t sliceSize = base + (i < remainder ? 1 : 0);
            lfuSliceCaches_.emplace_back(std::make_unique<LfuCache<Key, Value>>(sliceSize));
            lfuSliceCaches_.back()->setLeaveCallback(
                [this](const Key& k, const Value& v, LeaveReason r) {
                    // 分片在自身锁外分发事件，本类无外层锁，直接转发即可
                    this->notifyLeave(k, v, r);
                }
            );
        }
    }

    // 禁止拷贝和移动
    // 分片回调捕获本对象 this，移动会使闭包内指针悬垂，故禁止拷贝/移动
    ShardedLfuCache(const ShardedLfuCache&) = delete;
    ShardedLfuCache& operator=(const ShardedLfuCache&) = delete;
    ShardedLfuCache(ShardedLfuCache&&) = delete;
    ShardedLfuCache& operator=(ShardedLfuCache&&) = delete;

    void put(Key key, Value value) override {
        size_t sliceIndex = hash(key);
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(const Key& key, Value& value) override {
        size_t sliceIndex = hash(key);
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    void remove(const Key& key) override {
        size_t sliceIndex = hash(key);
        lfuSliceCaches_[sliceIndex]->remove(key);
    }

    size_t size() const override {
        size_t size = 0;
        for (const auto& cache : lfuSliceCaches_) {
            size += cache->size();
        }
        return size;
    }

    // 查询是否存在（key 经哈希只可能位于一个分片，直接查询对应分片）
    bool exists(const Key& key) const override { return lfuSliceCaches_[hash(key)]->exists(key); }

    // 用户设定的总容量（各分片容量之和）
    size_t capacity() const override { return capacity_; }

   private:
    size_t hash(const Key& key) const { return hashFunc_(key) % sliceNum_; }

    size_t capacity_;                                                    // 用户设定的总容量
    int sliceNum_;                                                       // 分片数量
    std::vector<std::unique_ptr<LfuCache<Key, Value>>> lfuSliceCaches_;  // 存储每一个LFU缓存分片
    HashFunc hashFunc_;                                                  // 哈希函数
};
}  // namespace louis::cache
