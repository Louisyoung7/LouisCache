// 命中率研究工具：不计时、只统计，与 bench_*.cc 的计时基准互补——
// 计时基准回答"多快"，本工具回答"多聪明"。这也是 LruK / Arc / Lfu
// 存在的理由：计时基准已证明它们速度同级，差异只能来自命中率。
//
// 实验设计：唯一变量是容量比例，其余全部固定。
// - trace 完全固定：6 种分布、固定种子、每条约 10 万次访问
// - capacity 从访问域（4096）的 1/64 扫到 1/1，得到经典命中率曲线
// - 统计口径：同一策略实例回放两遍，第一遍预热不计数（否则冷启动 miss
//   会污染小容量档），第二遍为稳态命中率
//
// 回放语义与 bench_workloads 一致：get 命中即止；miss 则 put 回填，
// 缓存写满后淘汰自然发生。这样两份工具的 hit_rate 才能横向对数。
//
// 用法：hitrate_study [csv 输出路径，默认 hitrate_results.csv]

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "policies.h"
#include "trace.h"

namespace {

using louis::bench::Key;
using louis::bench::Policy;
using louis::bench::Trace;
using louis::bench::Value;
using louis::bench::kCapacity;
using louis::bench::kSliceNum;
using louis::bench::kTraceSeed;
using louis::bench::kZipfExponent;
using louis::bench::kPolicyNames;
using louis::bench::makePolicy;

// ---- 实验常量 ----

// 每条 trace 约 10 万次访问：小容量档命中率低时，几千个样本的统计波动
// 会很明显，10 万个样本能把相对误差压在几个百分点内。
constexpr std::size_t kStudyTraceSize = 100000;
// LongScan 一个周期 = 2048 热点 + 1024 扫描 = 3072 条；33 个周期 ≈ 10.1 万条。
constexpr std::size_t kStudyScanCycles = 33;
// PhaseShift 一个周期 = 2 × 2048 = 4096 条；25 个周期 = 10.2 万条。
constexpr std::size_t kStudyPhaseCycles = 25;

// 访问域固定为 4 × 基准容量 = 4096，与 trace.h 的 kUniverse 同口径。
// 容量档按 2 倍递进：1/64 → 1/1，足够画出命中率曲线的形状。
constexpr std::size_t kStudyUniverse = 4 * kCapacity;
constexpr std::size_t kStudyCapacities[] = {64, 128, 256, 512, 1024, 2048, 4096};

// 研究专用 trace：与 makeTraces() 同一批分布，但长度拉到 10 万量级。
// ScanMix / LongScan / PhaseShift 的热点集与扫描跨度沿用原常量——
// 负载保持固定，扫的只有容量，这才符合"一次只动一个变量"。
std::vector<Trace> makeStudyTraces() {
    std::vector<Trace> traces;
    traces.push_back(
        louis::bench::makeUniformTrace(kTraceSeed, kStudyTraceSize, kStudyUniverse));
    traces.push_back(louis::bench::makeSequentialTrace(kStudyTraceSize, kStudyUniverse));
    traces.push_back(
        louis::bench::makeZipfTrace(kZipfExponent, kTraceSeed, kStudyTraceSize, kStudyUniverse));
    traces.push_back(louis::bench::makeScanMixTrace(kTraceSeed, kStudyTraceSize));
    traces.push_back(louis::bench::makeLongScanTrace(kTraceSeed, kStudyScanCycles));
    traces.push_back(
        louis::bench::makePhaseShiftTrace(kZipfExponent, kTraceSeed, kStudyPhaseCycles));
    return traces;
}

// 回放一条 trace，返回命中次数。get 命中即止；miss 则 put 回填。
std::uint64_t replay(const Trace& trace, Policy& policy) {
    std::uint64_t hits = 0;
    Value value = 0;
    for (Key key : trace.keys) {
        if (policy.get(key, value)) {
            ++hits;
        } else {
            policy.put(key, key);
        }
    }
    return hits;
}

// 预热一遍 + 数第二遍：第一遍在同一个策略实例上完整回放但不计数，
// 第二遍开始时缓存已处于稳态，测出的是持续行为而非冷启动。
double measureHitRate(const Trace& trace, const std::string& policyName, std::size_t capacity) {
    auto policy = makePolicy(policyName, kSliceNum, capacity);
    replay(trace, *policy);  // 预热
    const std::uint64_t hits = replay(trace, *policy);
    return 100.0 * static_cast<double>(hits) / static_cast<double>(trace.keys.size());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string csvPath = argc > 1 ? argv[1] : "hitrate_results.csv";
    std::FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (csv == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", csvPath.c_str());
        return 1;
    }
    // 长表格式（每行一个观测点），便于以后用任何画图工具直接读。
    std::fprintf(csv, "distribution,policy,capacity,hit_rate_pct\n");

    const std::vector<Trace> traces = makeStudyTraces();

    for (const Trace& trace : traces) {
        std::printf("== %s (trace=%zu) ==\n", std::string(trace.name).c_str(),
                    trace.keys.size());
        std::printf("%8s %7s", "capacity", "ratio");
        for (std::string_view name : kPolicyNames) {
            std::printf(" %10s", std::string(name).c_str());
        }
        std::printf("\n");

        for (std::size_t capacity : kStudyCapacities) {
            std::printf("%8zu %6.1f%%", capacity,
                        100.0 * static_cast<double>(capacity) /
                            static_cast<double>(kStudyUniverse));
            for (std::string_view name : kPolicyNames) {
                const double rate =
                    measureHitRate(trace, std::string(name), capacity);
                std::printf(" %9.2f%%", rate);
                std::fprintf(csv, "%s,%s,%zu,%.2f\n", std::string(trace.name).c_str(),
                             std::string(name).c_str(), capacity, rate);
            }
            std::printf("\n");
        }
        std::printf("\n");
    }

    std::fclose(csv);
    std::printf("CSV written to %s\n", csvPath.c_str());
    return 0;
}
