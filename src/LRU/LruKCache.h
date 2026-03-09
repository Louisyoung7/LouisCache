#pragma once

#include <memory>
#include <unordered_map>

#include "LRU/LruCache.h"

namespace louis::cache {
template <typename Key, typename Value>
class LruKCache : public LruCache<Key, Value> {
    std::unique_ptr<LruCache<Key, Value>> historyList_;  // 访问历史队列，Value存储访问次数
    std::unordered_map<Key, Value> valueMap_;            // 存储访问历史队列里的缓存项
    int k_;  // 从访问历史队列移动到主缓存的阈值，一般设置为2
   public:
    LruKCache(int capacity, int historyCapacity, int k = 2)
        : LruCache<Key, Value>(capacity), historyList_(std::make_unique<LruCache<Key, Value>>(historyCapacity)), k_(k) {
    }

    // 添加缓存项
    void put(Key key, Value value) override {
        Value existingValue{};
        bool inMain = LruCache<Key, Value>::get(key, existingValue);

        if (inMain) {
            // 如果在主缓存中，更新主缓存
            LruCache<Key, Value>::put(key, value);
        } else {
            // 不在主缓存中，添加到历史访问队列
            int accessCount = 0;
            // 如果在访问历史队列没有，get 返回的访问次数是0
            historyList_->get(key, accessCount);
            accessCount++;
            historyList_->put(key, accessCount);

            // 达到阈值，先从访问历史队列移除，再添加到主缓存中
            if (accessCount >= k_) {
                historyList_->remove(key);
                valueMap_.erase(key);
                LruCache<Key, Value>::put(key, value);
            }
        }
    }

    // 查询缓存项
    Value get(const Key& key) override {
        Value value{};

        // 优先从主缓存获取
        bool inMain = LruCache<Key, Value>::get(key, value);
        if (inMain) {
            return value;
        }

        int accessCount = 0;
        bool inHistory = historyList_->get(key, accessCount);
        if (!inHistory) {
            // 在访问历史队列里也没有，返回空值
            return value;
        }

        // 在访问历史队列中，更新访问次数
        accessCount++;
        historyList_->put(key, accessCount);

        // 达到阈值，移动到主缓存
        if (accessCount >= k_) {
            auto it = valueMap_.find(key);
            if (it != valueMap_.end()) {
                Value storedValue{};
                historyList_->get(key, storedValue);

                // 从访问历史队列移除
                historyList_->remove(key);
                valueMap_.erase(it);

                // 添加到主缓存
                LruCache<Key, Value>::put(key, storedValue);
            }
        }

        // 就算在访问历史队列中，也视为缓存未命中，返回空值
        return value;
    }
};
}  // namespace louis::cache