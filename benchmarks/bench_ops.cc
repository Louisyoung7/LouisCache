#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <string>

#include "policies.h"

namespace {

using louis::bench::Key;
using louis::bench::Policy;
using louis::bench::kCapacity;
using louis::bench::kWorkingSet;

// 写入热工作集；调用方负责把这些操作放在计时之外。
void fill(Policy& policy) {
    for (Key key = 0; key < kWorkingSet; ++key) {
        policy.put(key, key * 3);
    }
}

// LruK 需要达到访问阈值（默认 2）才进入主缓存；其他策略一次 put 即驻留。
// 因此 LruK 多预热一遍，保证 GetHit 场景下所有 key 确实在主缓存中。
void warmUp(Policy& policy, const std::string& name) {
    fill(policy);
    if (name == "LruK") {
        fill(policy);
    }
}

void setItems(benchmark::State& state) {
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kWorkingSet));
}

void benchGetHit(benchmark::State& state, Policy& policy, const std::string&) {
    std::uint64_t hits = 0;
    for (auto _ : state) {
        for (Key key = 0; key < kWorkingSet; ++key) {
            std::uint64_t value = 0;
            hits += policy.get(key, value) ? 1 : 0;
            benchmark::DoNotOptimize(value);
        }
        benchmark::DoNotOptimize(hits);
    }

    // 期望值：每次迭代 kWorkingSet 次全部命中；用于确认没有测错路径
    state.counters["hits_per_iter"] = static_cast<double>(hits) / static_cast<double>(state.iterations());
    setItems(state);
}

void benchGetMiss(benchmark::State& state, Policy& policy, const std::string&) {
    std::uint64_t hits = 0;
    for (auto _ : state) {
        for (Key key = kCapacity; key < kCapacity + kWorkingSet; ++key) {
            std::uint64_t value = 0;
            hits += policy.get(key, value) ? 1 : 0;
            benchmark::DoNotOptimize(value);
        }
        benchmark::DoNotOptimize(hits);
    }

    // 期望值：0；若大于 0 说明 miss 场景的 key 意外驻留
    state.counters["hits_per_iter"] = static_cast<double>(hits) / static_cast<double>(state.iterations());
    setItems(state);
}

void benchPutOverwrite(benchmark::State& state, Policy& policy, const std::string&) {
    for (auto _ : state) {
        for (Key key = 0; key < kWorkingSet; ++key) {
            policy.put(key, key * 3 + 1);
        }
        benchmark::ClobberMemory();
    }

    setItems(state);
}

void benchPutNew(benchmark::State& state, Policy& policy, const std::string&) {
    // 始终插入未出现过的 key：满容量后每轮都伴随淘汰。
    // 注意：LruK 的首次 put 只进历史队列，不进入主缓存，本场景测的是历史入队路径。
    std::uint64_t nextKey = kCapacity;
    for (auto _ : state) {
        for (Key i = 0; i < kWorkingSet; ++i) {
            const Key key = nextKey++;
            policy.put(key, key);
        }
        benchmark::ClobberMemory();
    }

    setItems(state);
}

void benchRemove(benchmark::State& state, Policy& policy, const std::string& name) {
    for (auto _ : state) {
        for (Key key = 0; key < kWorkingSet; ++key) {
            policy.remove(key);
        }

        // 恢复数据不计入计时；PauseTiming 必须在 range-for 循环内调用
        state.PauseTiming();
        warmUp(policy, name);
        state.ResumeTiming();
    }

    setItems(state);
}

void benchExists(benchmark::State& state, Policy& policy, const std::string&) {
    std::uint64_t found = 0;
    for (auto _ : state) {
        for (Key key = 0; key < kWorkingSet; ++key) {
            found += policy.exists(key) ? 1 : 0;
        }
        benchmark::DoNotOptimize(found);
    }

    // 期望值：每次迭代 kWorkingSet 次全部存在
    state.counters["found_per_iter"] = static_cast<double>(found) / static_cast<double>(state.iterations());
    setItems(state);
}

using OpFn = void (*)(benchmark::State&, Policy&, const std::string&);

struct Context {
    std::string name;
    std::unique_ptr<Policy> policy;
};

// Setup/Teardown 由 Google Benchmark 在计时之外调用，避免把构造与预热算进结果。
void registerOp(const std::string& policyName, const std::string& opName, OpFn fn) {
    auto ctx = std::make_shared<Context>();
    ctx->name = policyName;

    benchmark::RegisterBenchmark(policyName + "/" + opName,
                                 [ctx, fn](benchmark::State& state) { fn(state, *ctx->policy, ctx->name); })
        ->Setup([ctx](const benchmark::State&) {
            ctx->policy = louis::bench::makePolicy(ctx->name);
            warmUp(*ctx->policy, ctx->name);
        })
        ->Teardown([ctx](const benchmark::State&) { ctx->policy.reset(); });
}

void registerForPolicy(const std::string& name) {
    registerOp(name, "GetHit", benchGetHit);
    registerOp(name, "GetMiss", benchGetMiss);
    registerOp(name, "PutOverwrite", benchPutOverwrite);
    registerOp(name, "PutNew", benchPutNew);
    registerOp(name, "Remove", benchRemove);
    registerOp(name, "Exists", benchExists);
}

// 全局对象构造期完成注册，早于 main。
struct RegisterAll {
    RegisterAll() {
        for (auto name : louis::bench::kPolicyNames) {
            registerForPolicy(std::string(name));
        }
    }
};

const RegisterAll kRegisterAll{};

}  // namespace
