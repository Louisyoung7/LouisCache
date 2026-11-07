---
title: "多线程压测自建线程池而非用 Google Benchmark Threads(n)"
date: 2026-11-07
status: Accepted
---

# ADR-0001: 多线程压测自建线程池而非用 Google Benchmark Threads(n)

## 背景

LouisCache 的第三层压测（benchmarks/bench_threads.cc）测"锁结构在并发下的
伸缩性"：n 个线程同时回放 trace、共享同一缓存实例，对比全局锁与分片锁。
被测差异是纳秒级（如 S1 与 S16 在 8 线程下差数倍、单线程分片开销约 10%），
对 harness 自身的扰动极其敏感。

- CON-001: 测试机为 powersave governor（i9-13980HX，800 MHz–4.7 GHz 跳变），
  线程创建/首次唤醒会让 CPU 从低频态爬起。实测每轮新建线程使单线程
  从 54 ns/item 膨胀到 326（约 5 倍），完全淹没被测差异。
- CON-002: 实验口径是"一轮 = n 个工人同时开跑约 20 万次访问，测该轮墙钟"。
  需要跨线程的每迭代栅栏（所有工人同步开跑、全部完成后计时停止）。
- CON-003: 需要跨线程汇总的诊断计数（hit_rate_pct、busy_ns_per_item、
  evictions_per_iter）。Google Benchmark 的 state.counters 是每线程独立的，
  没有官方聚合机制。
- CON-004: threads=1 的基线必须与第二层回放（bench_workloads）逐指令同口径，
  才能支撑跨层对照；实测只留一个空闲 worker 就会让主线程耗时翻倍。

## 决策

在 bench_threads.cc 中自建常驻线程池 WorkerPool，围绕"每轮栅栏 + 按工人
索引的结果缓冲"组织测量。

- DEC-001: 线程池在 benchmark 的 Setup 中一次性创建并常驻，跨迭代复用，
  线程创建成本全部落在计时之外。
- DEC-002: 每轮同步用 std::mutex + 两个 condition_variable
  （startCv: 主线程→工人放行；doneCv: 工人→主线程汇合），
  不用忙等自旋。
- DEC-003: 结果按工人下标写入 hits[i]/misses[i]/busyNs[i]，
  由主线程在轮末统一汇总，保证诊断计数跨线程干净聚合。
- DEC-004: threads=1 不创建线程池，主线程直接调用与 bench_workloads
  相同的 runTrace，保证单线程基线跨层可比。
- DEC-005: 保留 busy_ns_per_item 自检计数（工人自测执行时长之和），
  正常应约等于 ns_per_item × 线程数，用于证明 CV 握手没有污染测量。

## 备选方案

### ALT-001: 使用 Google Benchmark 自带多线程 API（->Threads(n) / ThreadPerCpu()）

- 否决原因：每次运行新建并销毁 n 个线程（违反 CON-001）；各线程独立跑
  for (auto _ : state)，迭代间无跨线程栅栏，无法表达"同一轮"（CON-002）；
  自定义计数器每线程独立、无聚合（CON-003）。其模型适合"n 个线程各干各的"
  的持续吞吐测试，不适合每轮严格同步的锁争用实验。

### ALT-002: 工人忙等自旋等待下一轮

- 否决原因：自旋占满核心，主线程等待时与工人抢 CPU，实测单线程耗时
  膨胀近 3 倍；双条件变量握手成本已被 kTargetItemsPerIter=200000
  摊薄到 0.1% 量级（DEC-005 可验证）。

### ALT-003: 每轮迭代新建 std::thread

- 否决原因：同 CON-001，实测 5 倍膨胀（326 vs 54 ns/item），不可接受。
