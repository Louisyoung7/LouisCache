#pragma once

#include <algorithm>
#include <cmath>
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

// Zipf 偏斜指数：s 越大头部越集中。1.0 是常见的"自然语言/网页访问"近似。
inline constexpr double kZipfExponent = 1.0;

// Zipf 分布：第 r 名的概率正比于 1/r^s。用逆 CDF 采样，避免逐次重算权重。
// 头部热点 key 会被反复访问，因此能真正触发 LruK 的晋升路径。
inline Trace makeZipfTrace(double exponent = kZipfExponent,
                           std::uint64_t seed = kTraceSeed,
                           std::size_t size = kTraceSize,
                           std::size_t universe = kUniverse) {
    std::vector<double> cdf(universe);
    double sum = 0.0;
    for (std::size_t rank = 1; rank <= universe; ++rank) {
        sum += 1.0 / std::pow(static_cast<double>(rank), exponent);
        cdf[rank - 1] = sum;
    }
    for (auto& value : cdf) {
        value /= sum;
    }

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    Trace trace{"Zipf", {}};
    trace.keys.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        const double u = dist(rng);
        auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
        // rank 从 1 开始编号，key 用 0 起始；越界时钳到最后一个 key。
        std::size_t rank = static_cast<std::size_t>(it - cdf.begin()) + 1;
        if (rank > universe) {
            rank = universe;
        }
        trace.keys.push_back(static_cast<Key>(rank - 1));
    }
    return trace;
}

// 扫描混合分布：模拟"热点 + 周期性冷数据扫描"。
// - 热点集合取满容量（1024），让扫描插入必须挤掉热 key，污染才真实发生
// - 每 100 次访问 = 90 次热点 + 10 次连续冷扫描；必须是**连续突发**，
//   零散的单次扫描会被 LRU 立刻淘汰，测不出污染
// - 冷 key 从 kUniverse 起递增并回绕，与热点集合不重叠
inline constexpr std::size_t kScanHotSet = kCapacity;
inline constexpr std::size_t kScanColdSpan = kUniverse;
inline constexpr std::size_t kScanBlock = 100;
inline constexpr std::size_t kScanBurst = 10;

inline Trace makeScanMixTrace(std::uint64_t seed = kTraceSeed,
                              std::size_t size = kTraceSize) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Key> hot(0, static_cast<Key>(kScanHotSet - 1));

    Trace trace{"ScanMix", {}};
    trace.keys.reserve(size);
    std::size_t scanIndex = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (i % kScanBlock >= kScanBlock - kScanBurst) {
            trace.keys.push_back(
                static_cast<Key>(kUniverse + (scanIndex % kScanColdSpan)));
            ++scanIndex;
        } else {
            trace.keys.push_back(hot(rng));
        }
    }
    return trace;
}

// 长扫描分布：模拟"周期性的整表扫描"。
// - 每个周期 = 2048 次热点访问 + 1024 次连续冷扫描
// - 扫描块长度 = 容量 1024，足以把 LRU 的缓存整体冲掉，污染强度才够
// - 跑 5 个周期：第 1 个是预热，后 4 个用于观察"扫描之后能否恢复"
inline constexpr std::size_t kLongScanHotBlock = 2048;
inline constexpr std::size_t kLongScanScanBlock = kCapacity;
inline constexpr std::size_t kLongScanCycles = 5;

inline Trace makeLongScanTrace(std::uint64_t seed = kTraceSeed,
                               std::size_t cycles = kLongScanCycles) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Key> hot(0, static_cast<Key>(kScanHotSet - 1));

    Trace trace{"LongScan", {}};
    trace.keys.reserve(cycles * (kLongScanHotBlock + kLongScanScanBlock));

    std::size_t scanIndex = 0;
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        for (std::size_t i = 0; i < kLongScanHotBlock; ++i) {
            trace.keys.push_back(hot(rng));
        }
        for (std::size_t i = 0; i < kLongScanScanBlock; ++i) {
            trace.keys.push_back(
                static_cast<Key>(kUniverse + (scanIndex % kScanColdSpan)));
            ++scanIndex;
        }
    }
    return trace;
}

// 相位切换分布：模拟"访问模式在频率型与最近型之间来回切换"。
// - 相位 A：Zipf over key [0, 1024)，频率型（少数热点反复访问）
// - 相位 B：均匀 over key [1024, 2048)，最近型（平坦、工作集 = 容量）
// - 两组 key 不相交，每段 2048 次，交替 2 个周期
// - 用于检验 ARC 的幽灵队列反馈能否适应负载漂移，以及 LFU 是否会被陈旧频次拖累
inline constexpr std::size_t kPhaseShiftSet = kCapacity;
inline constexpr std::size_t kPhaseLen = 2048;
inline constexpr std::size_t kPhaseShiftCycles = 2;

inline Trace makePhaseShiftTrace(double exponent = kZipfExponent,
                                 std::uint64_t seed = kTraceSeed,
                                 std::size_t cycles = kPhaseShiftCycles) {
    std::vector<double> cdf(kPhaseShiftSet);
    double sum = 0.0;
    for (std::size_t rank = 1; rank <= kPhaseShiftSet; ++rank) {
        sum += 1.0 / std::pow(static_cast<double>(rank), exponent);
        cdf[rank - 1] = sum;
    }
    for (auto& value : cdf) {
        value /= sum;
    }

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> real(0.0, 1.0);
    std::uniform_int_distribution<Key> flat(0, static_cast<Key>(kPhaseShiftSet - 1));

    Trace trace{"PhaseShift", {}};
    trace.keys.reserve(cycles * 2 * kPhaseLen);

    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        // 相位 A：频率型，Zipf over [0, 1024)
        for (std::size_t i = 0; i < kPhaseLen; ++i) {
            const double u = real(rng);
            auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
            std::size_t rank = static_cast<std::size_t>(it - cdf.begin()) + 1;
            if (rank > kPhaseShiftSet) {
                rank = kPhaseShiftSet;
            }
            trace.keys.push_back(static_cast<Key>(rank - 1));
        }
        // 相位 B：最近型，均匀 over [1024, 2048)
        for (std::size_t i = 0; i < kPhaseLen; ++i) {
            trace.keys.push_back(
                static_cast<Key>(kPhaseShiftSet + flat(rng)));
        }
    }
    return trace;
}

inline std::vector<Trace> makeTraces() {
    std::vector<Trace> traces;
    traces.push_back(makeUniformTrace());
    traces.push_back(makeSequentialTrace());
    traces.push_back(makeZipfTrace());
    traces.push_back(makeScanMixTrace());
    traces.push_back(makeLongScanTrace());
    traces.push_back(makePhaseShiftTrace());
    return traces;
}

}  // namespace louis::bench
