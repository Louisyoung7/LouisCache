#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
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
using louis::bench::makePolicy;

// 冷启动预填充：写入 key 0..kCapacity-1，与 trace 的 key 分布无关。
// 所有策略用同一份预填充，保证起跑状态一致。
void prefill(Policy& policy) {
    for (Key key = 0; key < kCapacity; ++key) {
        policy.put(key, key);
    }
}

struct Context {
    std::string policyName;
    const Trace* trace = nullptr;

    std::unique_ptr<Policy> policy;
    // 由 LeaveCallback 累加；进入 benchReplay 时清零，因此每个 benchmark
    // 调用（含 repetitions 的每次重复）都是从 0 开始统计。
    std::uint64_t evictions = 0;
};

// 回放 trace：get 命中就结束；未命中则 put 回填，于是缓存写满后淘汰自然发生。
void benchReplay(benchmark::State& state, Context& ctx) {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    ctx.evictions = 0;

    for (auto _ : state) {
        // 每个迭代都从相同初始状态重新开始；否则上一轮回放的缓存内容会
        // 让下一轮变成"热缓存"，计数逐轮漂移。
        // PauseTiming 必须放在 range-for 循环内（放到循环外会污染整段计时）。
        state.PauseTiming();
        ctx.policy = makePolicy(ctx.policyName);
        prefill(*ctx.policy);
        ctx.policy->setLeaveCallback(
            [&ctx](const Key&, const Value&, louis::cache::LeaveReason reason) {
                if (reason == louis::cache::LeaveReason::Evicted) {
                    ++ctx.evictions;
                }
            });
        state.ResumeTiming();

        for (Key key : ctx.trace->keys) {
            Value value = 0;
            if (ctx.policy->get(key, value)) {
                ++hits;
                benchmark::DoNotOptimize(value);
            } else {
                ++misses;
                ctx.policy->put(key, key);
            }
        }
    }

    const double iters = static_cast<double>(state.iterations());
    const double itemsPerIter = static_cast<double>(ctx.trace->keys.size());

    state.counters["hits_per_iter"] = static_cast<double>(hits) / iters;
    state.counters["misses_per_iter"] = static_cast<double>(misses) / iters;
    state.counters["evictions_per_iter"] = static_cast<double>(ctx.evictions) / iters;
    // 回放结束时的驻留条目数：用于判断主缓存是否真的写满（未满则不会有淘汰）。
    state.counters["cache_size"] = static_cast<double>(ctx.policy->size());
    // 命中率（百分比）。
    state.counters["hit_rate_pct"] = 100.0 * static_cast<double>(hits) / (iters * itemsPerIter);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() *
                                                      ctx.trace->keys.size()));
}

void registerReplay(const std::string& policyName, const Trace& trace) {
    auto ctx = std::make_shared<Context>();
    ctx->policyName = policyName;
    ctx->trace = &trace;

    benchmark::RegisterBenchmark(
        policyName + "/Replay-" + std::string(trace.name),
        [ctx](benchmark::State& state) { benchReplay(state, *ctx); })
        ->Teardown([ctx](const benchmark::State&) { ctx->policy.reset(); });
}

// trace 在全局持有，生命周期覆盖所有 benchmark。
std::vector<Trace> gTraces = louis::bench::makeTraces();

// 全局对象构造期完成注册，早于 main。
struct RegisterAll {
    RegisterAll() {
        for (auto name : louis::bench::kPolicyNames) {
            for (const auto& trace : gTraces) {
                registerReplay(std::string(name), trace);
            }
        }
    }
};

const RegisterAll kRegisterAll{};

}  // namespace
