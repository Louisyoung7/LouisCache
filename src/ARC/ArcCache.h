#pragma once

#include <cstddef>
#include <memory>

#include "ARC/ArcLfuPart.h"
#include "ARC/ArcLruPart.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class ArcCache : public Policy<Key, Value> {
    size_t capacity_;                                  // 总缓存容量
    size_t transformThreshold_;                        // 转换阈值
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;  // LRU缓存部分
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;  // LFU缓存部分

   public:
    explicit ArcCache(size_t capacity, size_t transformThreshold = 2)
        : capacity_(capacity),
          transformThreshold_(transformThreshold),
          lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold)),
          lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity)) {
    }

    void put(Key key, Value value) {
        // 确保在幽灵缓存中不存在该缓存项
        // 同时动态变换容量
        checkGhostCaches(key);

        // 更新LRU缓存部分
        lruPart_->put(key, value);

        // 如果LFU缓存也存在，更新LFU缓存部分
        if (lfuPart_->contain(key)) {
            lfuPart_->put(key, value);
        }
    }

    bool get(const Key& key, Value& value) {
        // 确保在幽灵缓存中不存在该缓存项
        // 同时动态变换容量
        checkGhostCaches(key);

        bool shouldTransform = false;

        // 优先从LRU获取
        if (lruPart_->get(key, value, shouldTransform)) {
            // 如果达到转换阈值，添加到LFU部分
            if (shouldTransform) {
                lfuPart_->put(key, value);
            }

            return true;
        }

        // 如果LRU没有相应缓存项，再从LFU获取
        return lfuPart_->get(key, value);
    }

    Value get(const Key& key) {
        Value value{};
        get(key, value);
        return value;
    }

   private:
    // ARC的动态调整逻辑体现在此
    // 判断指定键出现在哪个部分的幽灵缓存中，并增加那部分缓存的容量，减少另一部分缓存的容量
    bool checkGhostCaches(const Key& key) {
        bool inGhost = false;
        if (lruPart_->checkGhost(key)) {
            if (lfuPart_->decreaseCapacity()) {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } else if (lfuPart_->checkGhost(key)) {
            if (lruPart_->decreaseCapacity()) {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }

        return inGhost;
    }
};
}  // namespace louis::cache