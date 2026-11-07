#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "policies.h"
#include "trace.h"

namespace {

using louis::bench::Key;
using louis::bench::Policy;
using louis::bench::Value;
using louis::bench::kCapacity;
using louis::bench::makePolicy;

// 最多测到 8 线程；trace 副本一次生成到位，按 Arg 取前 n 份。
inline constexpr std::size_t kMaxThreads = 8;

// 冷启动预填充：与 bench_workloads 保持同一口径，所有策略从相同状态起跑。
void prefill(Policy& policy) {
    for (Key key = 0; key < kCapacity; ++key) {
        policy.put(key, key);
    }
}

// 回放一条 trace repeatCount 遍；命中直接返回，未命中回填。
// 工人线程与（单线程时的）主线程共用这段代码，保证两边可比。
inline void runTrace(Policy* policy,
                     const std::vector<Key>& keys,
                     std::size_t repeatCount,
                     std::uint64_t& hit,
                     std::uint64_t& miss) {
    for (std::size_t r = 0; r < repeatCount; ++r) {
        for (Key key : keys) {
            Value value = 0;
            if (policy->get(key, value)) {
                ++hit;
                benchmark::DoNotOptimize(value);
            } else {
                ++miss;
                policy->put(key, key);
            }
        }
    }
}

// 常驻工作线程池。
// 不每轮新建线程：线程创建/唤醒会让 CPU 从低频态爬起来，单线程实测会被
// 放大到 5 倍以上（326 ns/item vs 54 ns/item），测出来的就不是锁争用了。
// 等待用条件变量而不是自旋：自旋会占满核心，主线程等待时与工人抢 CPU，
// 单线程下也会把耗时放大近 3 倍。
struct WorkerPool {
    const std::vector<std::vector<Key>>* traces = nullptr;
    Policy* policy = nullptr;  // 每轮由主线程在暂停区间更新
    std::size_t threadCount = 0;

    std::mutex mutex;
    std::condition_variable startCv;  // 主线程 -> 工人：开跑下一轮
    std::condition_variable doneCv;   // 工人 -> 主线程：本轮全部完成

    std::vector<std::thread> workers;
    std::vector<std::uint64_t> hits;
    std::vector<std::uint64_t> misses;
    std::vector<std::int64_t> busyNs;  // 诊断用：工人自己跑 trace 的实际耗时
    std::uint64_t round = 0;           // 主线程递增
    std::size_t finished = 0;          // 本轮已完成的工人数
    std::size_t repeats = 1;           // 每轮每个工人连跑几遍 trace

    bool stop = false;

    WorkerPool(std::size_t count, const std::vector<std::vector<Key>>& t)
        : traces(&t), threadCount(count), hits(count, 0), misses(count, 0),
          busyNs(count, 0) {
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers.emplace_back([this, i]() { run(i); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        startCv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void run(std::size_t index) {
        std::unique_lock<std::mutex> lock(mutex);
        std::uint64_t seen = 0;
        while (true) {
            startCv.wait(lock, [&]() { return stop || round != seen; });
            if (stop) {
                return;
            }
            seen = round;
            const std::size_t repeatCount = repeats;
            lock.unlock();

            // policy 指针与 leave 回调都由主线程在递增 round 之前写好。
            std::uint64_t hit = 0;
            std::uint64_t miss = 0;
            const auto busyStart = std::chrono::steady_clock::now();
            runTrace(policy, (*traces)[index], repeatCount, hit, miss);

            lock.lock();
            hits[index] = hit;
            misses[index] = miss;
            busyNs[index] += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - busyStart)
                                 .count();
            ++finished;
            if (finished == threadCount) {
                doneCv.notify_one();
            }
        }
    }

    // 主线程：清零本轮计数并放行所有工人。
    void startRound(std::size_t repeatCount) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = 0;
            repeats = repeatCount;
            ++round;
        }
        startCv.notify_all();
    }

    // 主线程：阻塞到本轮所有工人完成。
    void waitRound() {
        std::unique_lock<std::mutex> lock(mutex);
        doneCv.wait(lock, [&]() { return finished == threadCount; });
    }
};

struct SharedCtx {
    std::string policyName;
    // 分片数；0 表示非分片策略（Lru / Lfu 为单把全局锁）。
    int sliceNum = 0;
    // 每线程一条独立的同分布 trace（种子不同）。
    // 若所有线程回放同一条序列，它们会在同一时刻访问同一个 key，
    // 这种人为同步会放大锁争用，测出来的不是真实并发行为。
    std::vector<std::vector<Key>> perThreadTrace;

    std::unique_ptr<Policy> policy;  // 每迭代重建，所有线程共享同一实例
    std::unique_ptr<WorkerPool> pool;
    std::atomic<std::uint64_t> evictions{0};
};

// 单次迭代的目标总访问量：约 20 万次，单线程下约 11 ms。
// 够长到把条件变量唤醒开销（几十 µs）摊薄，又不至于让迭代过久。
inline constexpr std::size_t kTargetItemsPerIter = 200000;

// 多线程回放：一条 trace 由 n 个线程同时跑，共享同一个缓存实例。
// - Lru / Lfu：整个缓存一把锁（每个操作都要抢）
// - ShardedLru / ShardedLfu：按 key 哈希分 n 片，各自一把锁
// 因此本文件测的是"锁结构"在并发下的伸缩性，而不是缓存算法本身。
// 与 bench_workloads 的口径不同：这里缓存只在 Setup 预热一次，之后是稳态热缓存，
// 用于观察持续并发下的吞吐，不做每轮冷启动重置。
void benchThreads(benchmark::State& state, SharedCtx& ctx) {
    const int threads = static_cast<int>(state.range(0));
    const std::size_t traceLen = ctx.perThreadTrace.front().size();
    // 每个工人连跑几遍 trace，使单次迭代的总访问量接近目标值。
    const std::size_t repeats =
        std::max<std::size_t>(1, kTargetItemsPerIter /
                                  (static_cast<std::size_t>(threads) * traceLen));
    const double perIterItems = static_cast<double>(threads) *
                                static_cast<double>(traceLen) *
                                static_cast<double>(repeats);

    std::uint64_t totalHits = 0;
    std::uint64_t totalMisses = 0;
    std::uint64_t totalEvictions = 0;
    std::int64_t activeNs = 0;
    std::uint64_t iters = 0;

    // 稳态口径：缓存在 Setup 里预热一次，后续迭代都跑在同一个热缓存上。
    // 不再每轮 Pause/Resume 重建：Google 的计时列在 Pause 语义下与自测墙钟
    // 不一致，这里全程不暂停，两个口径才对得上。
    for (auto _ : state) {
        ctx.evictions.store(0, std::memory_order_relaxed);
        std::uint64_t iterHits = 0;
        std::uint64_t iterMisses = 0;

        const auto start = std::chrono::steady_clock::now();
        if (threads == 1) {
            // 单线程直接在主线程跑：此时不创建线程池。
            // 实测哪怕只留一个空闲 worker，主线程也会掉到 2 倍耗时，
            // 单线程基线与 bench_workloads 的回放口径必须一致。
            runTrace(ctx.policy.get(), ctx.perThreadTrace[0], repeats, iterHits, iterMisses);
        } else {
            ctx.pool->startRound(repeats);
            ctx.pool->waitRound();
        }
        const auto stop = std::chrono::steady_clock::now();
        activeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        ++iters;

        if (threads == 1) {
            totalHits += iterHits;
            totalMisses += iterMisses;
        } else {
            for (int t = 0; t < threads; ++t) {
                totalHits += ctx.pool->hits[static_cast<std::size_t>(t)];
                totalMisses += ctx.pool->misses[static_cast<std::size_t>(t)];
            }
        }
        totalEvictions += ctx.evictions.load(std::memory_order_relaxed);
    }

    const double totalItems = static_cast<double>(iters) * perIterItems;

    std::int64_t busyNsTotal = 0;
    if (ctx.pool) {
        for (int t = 0; t < threads; ++t) {
            busyNsTotal += ctx.pool->busyNs[static_cast<std::size_t>(t)];
        }
    }

    state.counters["iterations"] = static_cast<double>(iters);
    state.counters["repeats"] = static_cast<double>(repeats);
    // 分片数：0 表示单把全局锁（Lru / Lfu）。用于确认分片参数真的生效。
    state.counters["slices"] = static_cast<double>(ctx.sliceNum);
    state.counters["hits_per_iter"] = static_cast<double>(totalHits) / static_cast<double>(iters);
    state.counters["misses_per_iter"] =
        static_cast<double>(totalMisses) / static_cast<double>(iters);
    state.counters["evictions_per_iter"] =
        static_cast<double>(totalEvictions) / static_cast<double>(iters);
    state.counters["cache_size"] = static_cast<double>(ctx.policy->size());
    state.counters["hit_rate_pct"] = 100.0 * static_cast<double>(totalHits) / totalItems;
    // 口径：每线程每 key（get，未命中时再 put）的墙钟耗时，含锁等待。
    state.counters["ns_per_item"] = static_cast<double>(activeNs) / totalItems;
    state.counters["busy_ns_per_item"] =
        static_cast<double>(busyNsTotal) / totalItems;
    state.counters["items_per_sec"] = totalItems / (static_cast<double>(activeNs) * 1e-9);
    // 交给 Google 的 item 数是"每次迭代"的量，不是总数，
    // 它会自己乘 iterations 换算吞吐，用于和自测口径交叉校验。
    state.SetItemsProcessed(static_cast<std::int64_t>(perIterItems));
}

void registerThreads(const std::string& policyName,
                     int sliceNum,
                     const std::string& loadName,
                     std::vector<std::vector<Key>> perThreadTrace) {
    auto ctx = std::make_shared<SharedCtx>();
    ctx->policyName = policyName;
    ctx->sliceNum = sliceNum;
    ctx->perThreadTrace = std::move(perThreadTrace);

    // 分片策略把片数写进基准名（ShardedLru-S4/Threads-Zipf），
    // 便于按片数过滤，也让结果表能直接对上参数。
    std::string benchName =
        sliceNum > 0 ? policyName + "-S" + std::to_string(sliceNum) : policyName;
    benchName += "/Threads-" + loadName;

    benchmark::RegisterBenchmark(benchName,
                                [ctx](benchmark::State& state) { benchThreads(state, *ctx); })
        ->ArgName("threads")
        ->Arg(1)
        ->Arg(2)
        ->Arg(4)
        ->Arg(8)
        ->UseRealTime()
        ->Setup([ctx](const benchmark::State& state) {
            if (state.range(0) > 1) {
                ctx->pool = std::make_unique<WorkerPool>(
                    static_cast<std::size_t>(state.range(0)), ctx->perThreadTrace);
            }
            // 缓存只在 Setup 里建一次并预热：预热在计时之外，
            // 后续迭代都跑在同一个热缓存上，测持续并发吞吐。
            ctx->policy = makePolicy(ctx->policyName, ctx->sliceNum);
            prefill(*ctx->policy);
            ctx->policy->setLeaveCallback(
                [ctx](const Key&, const Value&, louis::cache::LeaveReason reason) {
                    if (reason == louis::cache::LeaveReason::Evicted) {
                        ctx->evictions.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            if (ctx->pool) {
                ctx->pool->policy = ctx->policy.get();
            }
        })
        ->Teardown([ctx](const benchmark::State&) {
            ctx->pool.reset();
            ctx->policy.reset();
        });
}

std::vector<std::vector<Key>> makePerThreadZipf() {
    std::vector<std::vector<Key>> out;
    out.reserve(kMaxThreads);
    for (std::size_t t = 0; t < kMaxThreads; ++t) {
        auto trace = louis::bench::makeZipfTrace(louis::bench::kZipfExponent,
                                                 louis::bench::kTraceSeed +
                                                     static_cast<std::uint64_t>(1000 * t));
        out.push_back(std::move(trace.keys));
    }
    return out;
}

std::vector<std::vector<Key>> makePerThreadUniform() {
    std::vector<std::vector<Key>> out;
    out.reserve(kMaxThreads);
    for (std::size_t t = 0; t < kMaxThreads; ++t) {
        auto trace = louis::bench::makeUniformTrace(
            louis::bench::kTraceSeed + static_cast<std::uint64_t>(1000 * t));
        out.push_back(std::move(trace.keys));
    }
    return out;
}

// 全局对象构造期完成注册，早于 main。
struct RegisterAll {
    RegisterAll() {
        // 唯一变量：锁结构。同一家族内缓存算法相同，差异只来自"1 把锁 vs n 把锁"。
        // 两个算法家族各带一套"全局锁基线 + 分片组"，用于验证分片伸缩性
        // 是否是分片实现的性质（与缓存算法无关）：
        // - Lru 家族：Lru（全局锁，sliceNum = 0）+ ShardedLru-S1..S16
        // - Lfu 家族：Lfu（全局锁）+ ShardedLfu-S1..S16
        // 分片数取 1 / 2 / 4 / 8 / 16：1 片应与全局锁版本几乎重合（验证分片开销本身），
        // 4 片是产品默认值，16 片则超过最大线程数 8，用于观察"分片数 > 线程数"时
        // 是否仍有吞吐收益。
        const int kSliceSizes[] = {1, 2, 4, 8, 16};
        for (int sliceNum : kSliceSizes) {
            registerThreads("ShardedLru", sliceNum, "Zipf", makePerThreadZipf());
            registerThreads("ShardedLru", sliceNum, "Uniform", makePerThreadUniform());
            registerThreads("ShardedLfu", sliceNum, "Zipf", makePerThreadZipf());
            registerThreads("ShardedLfu", sliceNum, "Uniform", makePerThreadUniform());
        }
        registerThreads("Lru", 0, "Zipf", makePerThreadZipf());
        registerThreads("Lru", 0, "Uniform", makePerThreadUniform());
        registerThreads("Lfu", 0, "Zipf", makePerThreadZipf());
        registerThreads("Lfu", 0, "Uniform", makePerThreadUniform());
    }
};

const RegisterAll kRegisterAll{};

}  // namespace
