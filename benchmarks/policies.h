#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ARC/ArcCache.h"
#include "LFU/LfuCache.h"
#include "LFU/ShardedLfuCache.h"
#include "LRU/LruCache.h"
#include "LRU/LruKCache.h"
#include "LRU/ShardedLruCache.h"
#include "Policy.h"

namespace louis::bench {

using Key = std::uint64_t;
using Value = std::uint64_t;
using Policy = louis::cache::Policy<Key, Value>;

// 容量口径：所有策略共享同一用户容量 1024。
// - LruK 的历史队列另配 1024（历史队列不计入用户容量，也不参与主缓存命中）
// - 分片实现按 kSliceNum 均分总容量，各分片之和恰为 1024
// - ARC 的 T1 + T2 主缓存总量为 1024，幽灵缓存不占用户容量
inline constexpr std::size_t kCapacity = 1024;
inline constexpr std::size_t kLruKHistoryCapacity = 1024;
inline constexpr int kSliceNum = 4;

// 热工作集取容量的一半：保证预热后所有策略都能完整驻留。
// ARC 初始把 T1 目标容量设为 capacity/2，若一次写入满容量会有一半进幽灵缓存，
// 导致 GetHit/Exists 实测混入未命中；本轮只比较操作路径开销，不引入命中率因素。
inline constexpr std::size_t kWorkingSet = kCapacity / 2;

inline constexpr std::string_view kPolicyNames[] = {
    "Lru", "LruK", "ShardedLru", "Lfu", "ShardedLfu", "Arc",
};

// sliceNum 只对分片策略生效，其余策略忽略该参数。
// capacity 允许调用方覆盖默认容量（命中率研究按容量比例扫描）；
// 各策略沿用 kCapacity 注释里的容量口径，LruK 的历史队列跟随主容量。
inline std::unique_ptr<Policy> makePolicy(std::string_view name, int sliceNum = kSliceNum,
                                          std::size_t capacity = kCapacity) {
    if (name == "Lru") {
        return std::make_unique<louis::cache::LruCache<Key, Value>>(static_cast<int>(capacity));
    }
    if (name == "LruK") {
        return std::make_unique<louis::cache::LruKCache<Key, Value>>(capacity, capacity);
    }
    if (name == "ShardedLru") {
        return std::make_unique<louis::cache::ShardedLruCache<Key, Value>>(capacity, sliceNum);
    }
    if (name == "Lfu") {
        return std::make_unique<louis::cache::LfuCache<Key, Value>>(capacity);
    }
    if (name == "ShardedLfu") {
        return std::make_unique<louis::cache::ShardedLfuCache<Key, Value>>(capacity, sliceNum);
    }
    if (name == "Arc") {
        return std::make_unique<louis::cache::ArcCache<Key, Value>>(capacity);
    }
    throw std::invalid_argument("unknown policy: " + std::string(name));
}

}  // namespace louis::bench
