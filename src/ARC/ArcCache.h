#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>

#include "ARC/ArcLfuPart.h"
#include "ARC/ArcLruPart.h"
#include "Policy.h"

namespace louis::cache {
// ARC (Adaptive Replacement Cache)
// 两个主缓存 T1（LRU 部分，倾向近期性）与 T2（LFU 部分，倾向频次）共享总容量，
// 由一个自适应参数 p 决定二者的目标容量划分；命中幽灵缓存时按两侧幽灵缓存大小的
// 比例调整 p，从而自动在“近期性”与“频次”之间倾斜
template <typename Key, typename Value>
class ArcCache : public Policy<Key, Value> {
   public:
    // 两个部分均空构造（初始容量为 0），
    // 主缓存与幽灵缓存的容量划分统一由 applyPartitions() 按自适应参数 p 决定
    explicit ArcCache(size_t capacity, size_t transformThreshold = 2)
        : capacity_(capacity),
          // p 为 T1 的目标容量。原论文中 p 初值为 0，本实现取均分，
          // 以免初始时某一侧目标容量为 0
          p_(capacity / 2),
          transformThreshold_(transformThreshold),
          lruPart_(std::make_unique<ArcLruPart<Key, Value>>(transformThreshold)),
          lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>()) {
        applyPartitions();
    }

    void put(Key key, Value value) override {
        if (capacity_ == 0) return;  // 容量 0：无处可存，直接丢弃

        std::lock_guard<std::mutex> lock(mutex_);
        // 确保在幽灵缓存中不存在该缓存项
        // 同时按 ARC 规则动态调整容量划分
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
    // 按当前 p 把总容量划分给两个主缓存与各自对应的幽灵缓存
    // 主缓存：|T1| <= p、|T2| <= capacity_ - p，两者之和恰为总容量
    // （此前两部分各自按总容量限额，实际可驻留 2 倍容量的条目）
    // 幽灵缓存：|B1| <= capacity_ - p、|B2| <= p，使 ARC 的不变量
    // |T1| + |B1| <= capacity_ 与 |T2| + |B2| <= capacity_ 成立
    void applyPartitions() {
        // p 在原论文中可取 [0, capacity_]，但本实现下主缓存容量是硬上限，
        // p 取端点会让某一侧无法驻留条目，故两侧各保留至少 1 个位置
        if (capacity_ >= 2) {
            p_ = std::min(std::max(p_, size_t{1}), capacity_ - 1);
        } else {
            p_ = capacity_;
        }

        lruPart_->setCapacity(p_);
        lfuPart_->setCapacity(capacity_ - p_);
        lruPart_->setGhostCapacity(capacity_ - p_);
        lfuPart_->setGhostCapacity(p_);
    }

    // ARC 的自适应调整逻辑体现在此
    // 命中 B1（该键因“不够新”被淘汰）→ 近期性获胜，增大 p
    // 命中 B2（该键因“不够频繁”被淘汰）→ 频次获胜，减小 p
    // 增量 δ 取两侧幽灵缓存大小之比的向上取整、且至少为 1；
    // 因命中后该键已从幽灵缓存移除，故先取命中前的大小
    void checkGhostCaches(const Key& key) {
        const size_t b1 = lruPart_->ghostSize();
        const size_t b2 = lfuPart_->ghostSize();

        if (lruPart_->tryToRemoveGhost(key)) {
            // δ1 = max(ceil(|B2| / |B1|), 1)；命中 B1 保证 |B1| >= 1
            const size_t delta = b1 >= b2 ? 1 : (b2 + b1 - 1) / b1;
            p_ = std::min(p_ + delta, capacity_);
            applyPartitions();
        } else if (lfuPart_->tryToRemoveGhost(key)) {
            // δ2 = max(ceil(|B1| / |B2|), 1)；命中 B2 保证 |B2| >= 1
            const size_t delta = b2 >= b1 ? 1 : (b1 + b2 - 1) / b2;
            p_ = delta > p_ ? 0 : p_ - delta;
            applyPartitions();
        }
    }

    size_t capacity_;  // 总缓存容量
    size_t p_;  // T1（LRU 部分）的目标容量，取值随幽灵缓存命中自适应调整
    size_t transformThreshold_;                        // 转换阈值
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;  // LRU缓存部分
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;  // LFU缓存部分

    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
};
}  // namespace louis::cache