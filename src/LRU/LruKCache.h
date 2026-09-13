#pragma once

#include <cstddef>
#include <mutex>
#include <optional>

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
        std::lock_guard<std::mutex> lock(mutex_);
        if (mainCache_.contains(key).has_value()) {  // 在主缓存中，更新主缓存
            mainCache_.put(key, std::move(value));
            return;
        }
        // 不在主缓存中：新值必须写进条目，否则历史里的旧值会覆盖本次 put
        HistoryEntry entry = historyList_.contains(key).value_or(HistoryEntry{});
        entry.value = std::move(value);
        if (auto promoted = recordAccess(key, std::move(entry))) {
            mainCache_.put(key, std::move(*promoted));
        }
    }

    // 获取缓存项
    // 若在主缓存中：直接返回
    // 若不在主缓存中：从访问历史队列中获取，更新访问次数，判断是否晋升到主缓存
    bool get(const Key& key, Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mainCache_.get(key, value)) return true;

        auto inHistory = historyList_.contains(key);
        if (!inHistory) return false;
        if (auto promoted = recordAccess(key, std::move(*inHistory))) {
            mainCache_.put(key, std::move(*promoted));
        }
        return false;  // 只要不是在主缓存中，均视为未命中
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
        std::lock_guard<std::mutex> lock(mutex_);
        mainCache_.remove(key);
        historyList_.remove(key);
    }

    // 获取缓存项数量
    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return mainCache_.size();
    }

   private:
    struct HistoryEntry {
        Value value;
        int accessCount{};
    };

    // 记录一次访问：未达阈值则写回历史队列并返回空；达到阈值则从历史队列移除并返回待晋升的值
    std::optional<Value> recordAccess(const Key& key, HistoryEntry entry) {
        if (++entry.accessCount >= k_) {
            historyList_.remove(key);
            return std::move(entry.value);
        }
        historyList_.put(key, std::move(entry));
        return std::nullopt;
    }

    LruCache<Key, Value> mainCache_;           // 主缓存
    LruCache<Key, HistoryEntry> historyList_;  // 访问历史队列
    int k_;  // 从访问历史队列移动到主缓存的阈值，一般设置为2
    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
};
}  // namespace louis::cache