#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>
#include <thread>
#include <cmath>

#include "LRU/LruCache.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value, typename HashFunc = std::hash<Key>>
class ShardedLruCache : public Policy<Key, Value> {
    size_t capacity_;                                                    // 总容量
    int sliceNum_;                                                       // 分片数量
    std::vector<std::unique_ptr<LruCache<Key, Value>>> lruSliceCaches_;  // 存储每一个LRU缓存分片的向量
    HashFunc hashFunc_;                                                  // 哈希函数

   public:
    ShardedLruCache(size_t capacity, int sliceNum = std::thread::hardware_concurrency())
        : capacity_(capacity), sliceNum_(sliceNum) {
        // 获取每个分片的大小
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_));
        for (int i = 0; i < sliceNum_; ++i) {
            lruSliceCaches_.emplace_back(std::make_unique<LruCache<Key, Value>>(sliceSize));
        }
    }

    void put(Key key, Value value) override {
        size_t sliceIndex = hashFunc_(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(const Key& key, Value& value) override {
        size_t sliceIndex = hashFunc_(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(const Key& key) {
        Value value{};
        get(key, value);
        return value;
    }
   private:
    // 获取 Key 的哈希值
    // 一般用于获取添加和查询缓存项的索引
    size_t hash(Key key) {
        return hashFunc_(key);
    }
};
}  // namespace louis::cache