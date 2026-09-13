#pragma once

#include <cstddef>

#include "LRU/LruCache.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class LruKCache : public Policy<Key, Value> {
   public:
    LruKCache(size_t capacity, size_t historyCapacity, int k = 2)
        : mainCache_(capacity), historyList_(historyCapacity), k_(k) {}

    // 添加缓存项
    // 若在主缓存中：直接更新
    // 若不在主缓存中：在历史队列中累计访问次数，达到 k次后晋升到主缓存
    void put(Key key, Value value) override {
        if (mainCache_.contains(key).has_value()) {  // 在主缓存中，更新主缓存
            mainCache_.put(key, std::move(value));
            return;
        }  // 不在主缓存中，更新访问历史
        std::optional<HistoryEntry> inHistory = historyList_.contains(key);
        HistoryEntry historyEntry{};
        historyEntry.value = std::move(value);
        historyEntry.accessCount =
            inHistory ? inHistory->accessCount + 1
                      : 1;  // 达到阈值，晋升到主缓存；否则留在历史队列中等待后续访问
        if (historyEntry.accessCount >= k_) {
            historyList_.remove(key);
            mainCache_.put(std::move(key), std::move(historyEntry.value));
        } else {
            historyList_.put(std::move(key), std::move(historyEntry));
        }
    }

    // 获取缓存项
    // 若在主缓存中：直接返回
    // 若不在主缓存中：从访问历史队列中获取，更新访问次数，判断是否晋升到主缓存
    bool get(const Key& key, Value& value) override {
        // 优先从主缓存获取
        bool inMain = mainCache_.get(key, value);
        if (inMain) return true;

        // 从访问历史队列获取
        std::optional<HistoryEntry> inHistory = historyList_.contains(key);
        if (inHistory) {
            HistoryEntry& historyEntry = inHistory.value();
            // 更新访问次数
            // 如果访问次数达到阈值，晋升到主缓存
            historyEntry.accessCount++;
            if (historyEntry.accessCount >= k_) {
                historyList_.remove(key);
                mainCache_.put(std::move(key), std::move(historyEntry.value));
            }
        }
        // 只要不是在主缓存中，均视为未命中，返回 false
        return false;
    }

    // 获取缓存项
    // 若在主缓存中：直接返回
    // 若不在主缓存中：从访问历史队列中获取，更新访问次数，判断是否晋升到主缓存
    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    // 显式移除缓存项
    // 若在主缓存中：直接移除
    // 若不在主缓存中：从访问历史队列中移除
    void remove(const Key& key) override {
        mainCache_.remove(key);
        historyList_.remove(key);
    }

    // 获取缓存项数量
    size_t size() const override { return mainCache_.size(); }

   private:
    struct HistoryEntry {
        Value value;
        int accessCount{};
    };

    LruCache<Key, Value> mainCache_;           // 主缓存
    LruCache<Key, HistoryEntry> historyList_;  // 访问历史队列
    size_t capacity_;                          // 主缓存容量
    size_t historyCapacity_;                   // 访问历史队列容量
    int k_;  // 从访问历史队列移动到主缓存的阈值，一般设置为2
};
}  // namespace louis::cache