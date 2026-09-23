#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>

#include "ARC/ArcLfuPart.h"
#include "ARC/ArcLruPart.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class ArcCache : public Policy<Key, Value> {
   public:
    explicit ArcCache(size_t capacity, size_t transformThreshold = 2)
        : capacity_(capacity),
          transformThreshold_(transformThreshold),
          lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold)),
          lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity)) {}

    void put(Key key, Value value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // 确保在幽灵缓存中不存在该缓存项
        // 同时动态变换容量
        checkGhostCaches(key);

        bool inLru = lruPart_->contain(key);
        bool inLfu = lfuPart_->contain(key);
        // 不变量：一个缓存项不同时存在于 LRU 与 LFU 部分
        assert(!(inLru && inLfu));

        // 已晋升：直接更新 LFU 部分，不再参与 LRU 的转换判定
        if (inLfu) {
            lfuPart_->put(key, value);
            return;
        }

        // 未晋升：写入 LRU 部分。已有条目由 put 内部累加访问次数并给出转换判定；
        // 新条目默认留在 LRU 部分累积访问次数，仅当阈值 <= 1 时才需要立即提升
        bool shouldTransform = false;
        lruPart_->put(key, value, shouldTransform);
        if (!inLru) {
            shouldTransform = transformThreshold_ <= 1;
        }

        // 达到阈值：迁移到 LFU 部分，保证条目在两部分中只有一份
        if (shouldTransform) {
            lruPart_->remove(key);
            lfuPart_->put(key, value);
        }
    }

    bool get(const Key& key, Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // 确保在幽灵缓存中不存在该缓存项
        // 同时动态变换容量
        checkGhostCaches(key);

        bool shouldTransform = false;

        // 优先从LRU获取
        if (lruPart_->get(key, value, shouldTransform)) {
            // 如果达到转换阈值，迁移到LFU部分，保证条目在两部分中只有一份
            if (shouldTransform) {
                lruPart_->remove(key);
                lfuPart_->put(key, value);
            }
            return true;
        }

        // 如果LRU没有相应缓存项，再从LFU获取
        return lfuPart_->get(key, value);
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    void remove(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // 移除LRU缓存部分
        lruPart_->remove(key);

        // 如果LFU缓存也存在，移除LFU缓存部分
        if (lfuPart_->contain(key)) {
            lfuPart_->remove(key);
        }
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lruPart_->size() + lfuPart_->size();
    }

   private:
    // ARC的动态调整逻辑体现在此
    // 判断指定键出现在哪个部分的幽灵缓存中，并增加那部分缓存的容量，减少另一部分缓存的容量
    void checkGhostCaches(const Key& key) {
        if (lruPart_->tryToRemoveGhost(key)) {
            if (lfuPart_->decreaseCapacity()) {
                lruPart_->increaseCapacity();
            }
        } else if (lfuPart_->tryToRemoveGhost(key)) {
            if (lruPart_->decreaseCapacity()) {
                lfuPart_->increaseCapacity();
            }
        }
    }

    size_t capacity_;                                  // 总缓存容量
    size_t transformThreshold_;                        // 转换阈值
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;  // LRU缓存部分
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;  // LFU缓存部分

    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
};
}  // namespace louis::cache