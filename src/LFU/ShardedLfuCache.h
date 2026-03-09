#pragma once

#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "LFU/LfuAgingCache.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value, typename HashFunc = std::hash<Key>>
class ShardedLfuCache : public Policy<Key, Value> {
    size_t capacity_;                                                         // 总缓存容量
    int sliceNum_;                                                            // 分片数量
    std::vector<std::unique_ptr<LfuAgingCache<Key, Value>>> lfuSliceCaches_;  // 存储每一个缓存分片的向量
    HashFunc hashFunc_;                                                       // 哈希函数

   public:
    explicit ShardedLfuCache(size_t capacity, int sliceNum = std::thread::hardware_concurrency(),
                             int maxAvgFreq = 100000)
        : capacity_(capacity), sliceNum_(sliceNum) {
        // 计算每一个分片的容量
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_));

        // 填充向量
        for (int i = 0; i < sliceNum_; ++i) {
            lfuSliceCaches_.emplace_back(std::make_unique<LfuAgingCache<Key, Value>>(sliceSize, maxAvgFreq));
        }
    }

    void put(Key key, Value value) override {
        size_t sliceIndex = hash(key) % sliceNum_;
        lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(const Key& key, Value& value) override {
        size_t sliceIndex = hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

   private:
    size_t hash(Key key) {
        return hashFunc_(key);
    }
};
}  // namespace louis::cache