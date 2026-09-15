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
        : mainCache_(capacity), historyList_(historyCapacity), k_(k) {
        mainCache_.setLeaveCallback([this](const Key& k, const Value& v, LeaveReason r) {
            // mainCache_ 中的缓存项被驱逐时，将 Key、Value、LeaveReason 打包记录到 pendingLeaves_
            // 此回调只会在外层公共方法的临界区内触发（mainCache_ 仅被它们触碰），
            // pendingLeaves_ 由外层锁保护，无需额外加锁
            pendingLeaves_.push_back({k, v, r});
        });
    }

    // 添加缓存项
    // 若在主缓存中：直接更新
    // 若不在主缓存中：在历史队列中累计访问次数，达到 k 次后晋升到主缓存
    void put(Key key, Value value) override {
        std::vector<LeaveEvent> toNotify;
        {
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
                // 晋升路径的 mainCache_.put 可能写入 pendingLeaves_
                toNotify = takePendingLeaves();
            }
        }
        notifyAll(toNotify);  // 锁外分发
    }

    // 获取缓存项
    // 若在主缓存中：直接返回
    // 若不在主缓存中：从访问历史队列中获取，更新访问次数，判断是否晋升到主缓存
    bool get(const Key& key, Value& value) override {
        std::vector<LeaveEvent> toNotify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (mainCache_.get(key, value)) return true;

            auto inHistory = historyList_.contains(key);
            if (!inHistory) return false;
            if (auto promoted = recordAccess(key, std::move(*inHistory))) {
                mainCache_.put(key, std::move(*promoted));
                // 晋升路径的 mainCache_.put 可能写入 pendingLeaves_
                toNotify = takePendingLeaves();
            }
        }
        notifyAll(toNotify);  // 锁外分发
        return false;         // 只要不是在主缓存中，均视为未命中
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
        std::vector<LeaveEvent> toNotify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mainCache_.remove(key);
            historyList_.remove(key);
            // mainCache_.remove 写入 pendingLeaves_
            toNotify = takePendingLeaves();
        }
        notifyAll(toNotify);  // 锁外分发
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

    struct LeaveEvent {
        Key key;
        Value value;
        LeaveReason reason;
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

    // 锁内调用：取走本轮收集的事件（出锁前必须调用，否则事件残留到下次）
    std::vector<LeaveEvent> takePendingLeaves() {
        std::vector<LeaveEvent> events = std::move(pendingLeaves_);
        pendingLeaves_.clear();
        return events;
    }

    // 锁外调用：逐条分发，用户回调内重入 put/get/size 均安全
    void notifyAll(std::vector<LeaveEvent>& events) {
        for (auto& e : events) this->notifyLeave(e.key, e.value, e.reason);
    }

    LruCache<Key, Value> mainCache_;           // 主缓存
    LruCache<Key, HistoryEntry> historyList_;  // 访问历史队列
    int k_;  // 从访问历史队列移动到主缓存的阈值，一般设置为2
    mutable std::mutex mutex_;               // size() 为 const，需 mutable 才能加锁
    std::vector<LeaveEvent> pendingLeaves_;  // 仅在外层临界区内访问
};
}  // namespace louis::cache