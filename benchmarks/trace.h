#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

#include "policies.h"

namespace louis::bench {

// 单次回放长度：够长到让命中/淘汰比例稳定，又不至于让一个迭代过久。
inline constexpr std::size_t kTraceSize = 4096;

// 访问域 = 4 倍容量：工作集明显大于缓存容量，miss 与淘汰才会真实发生。
inline constexpr std::size_t kUniverse = 4 * kCapacity;

// 固定种子：所有策略回放完全相同的 key 序列，结果可复现、可横向比较。
inline constexpr std::uint64_t kTraceSeed = 0xC0FFEEULL;

struct Trace {
    std::string_view name;
    std::vector<Key> keys;
};

// 均匀随机分布：key 在 [0, universe) 内等概率抽取。
inline Trace makeUniformTrace(std::uint64_t seed = kTraceSeed,
                              std::size_t size = kTraceSize,
                              std::size_t universe = kUniverse) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Key> dist(0, static_cast<Key>(universe - 1));

    Trace trace{"Uniform", {}};
    trace.keys.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        trace.keys.push_back(dist(rng));
    }
    return trace;
}

// 顺序分布：key 从 0 递增到 universe-1 后回绕，模拟扫描 + 循环复用。
inline Trace makeSequentialTrace(std::size_t size = kTraceSize,
                                 std::size_t universe = kUniverse) {
    Trace trace{"Sequential", {}};
    trace.keys.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        trace.keys.push_back(static_cast<Key>(i % universe));
    }
    return trace;
}

inline std::vector<Trace> makeTraces() {
    std::vector<Trace> traces;
    traces.push_back(makeUniformTrace());
    traces.push_back(makeSequentialTrace());
    return traces;
}

}  // namespace louis::bench
