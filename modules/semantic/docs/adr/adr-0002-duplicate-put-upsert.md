---
title: "SemanticCache 重复 put 采用 exactIndex_ 覆盖写（upsert）"
date: 2026-10-29
status: Accepted
---

# ADR-0002: SemanticCache 重复 put 采用 exactIndex_ 覆盖写（upsert）

## 背景

同一 prompt 被多次写入是缓存的正常用法（修正答案、刷新内容）。`put` 若不处理重复，会在 `VectorIndex` 中留下多条余弦恒为 1 的向量，语义路径的返回结果就取决于并列分的排序规则，而不是"最新写入应当生效"。

- CON-001: 重复 put 必须满足语义幂等：同 prompt 只保留最新 `response`，且不占两份容量。
- CON-002: `VectorIndex` 没有"按向量反查 id"的接口，`search` 只按余弦排序，并列分按 id 升序返回；同一 prompt 的多次写入恰好会产生并列分。
- CON-003: `VectorIndex` 的 id 由 `nextId_.fetch_add` 原子单调分配，新写入的 id 恒大于旧 id，但该单调性不等于"该条目仍在 store 内"。
- CON-004: 精确命中路径已经需要 `prompt → id` 的内联映射 `exactIndex_`（设计文档 §2.1）；写入侧若不复用同一结构，就要再引入第二个判重结构并保证两者一致。
- CON-005: 接口契约要求索引与存储同量（无幽灵命中），任何条目离场都必须经 `onLeave` 单一出口，替换也不能例外。
- CON-006: `mu_` 只保护 `exactIndex_` / `stats_`，不得跨 `store_` / `vectorIndex_` 调用持锁；`onLeave` 在 store 锁外被调用。
- CON-007: 阶段 1 不追求并发严格幂等：两个线程对同一 prompt 同时 put 允许出现双插（已知限制）。

## 决策

- DEC-001: 重复判定只以 `exactIndex_` 为唯一依据。`put` 首步在 `mu_` 内 `findExactLocked(prompt)`，命中即先 `store_->remove(oldId)`，让旧条目走 `LeaveReason::Explicit` 离场。
- DEC-002: 覆盖写走统一写入序列：`nextId_.fetch_add` → `store_->put` → `exists(nextId)` 复核 → `vectorIndex_->add` → 锁内 `exactIndex_[prompt] = nextId`。
- DEC-003: 旧条目的清理交给 `onLeave`：`vectorIndex_->remove(id)` 后，用 `eraseExactIfMatchesLocked(entry.prompt, id)` 仅当映射仍指向该 id 才擦除，避免误删同 prompt 的新映射。
- DEC-004: 替换属于显式删除，只计入 `stats_.puts`，不计 `evictions` / `expirations`；这也是"不占两份容量"的可观测判据。
- DEC-005: `exactIndex_` 的读取（find）、写入（`operator[]`）、条件删除三处访问全部由 `mu_` 串行化，且三处都不得包住 store/index 调用。

## 备选方案

### ALT-001: 用 `vectorIndex_->search(embed(prompt), 1)` 判重

- 否决原因：与 CON-002 冲突，索引没有按向量反查 id 的能力，只能借 `search` 的分数间接判断；而分数低于 `hitThreshold` 时会把"同字面重复"误判为"新条目"，判重正确性被检索阈值污染。每次 put 还会多一次 O(N·dim) 全量扫描，与 CON-001 的"顺手去重"目标不匹配。

### ALT-002: 允许并存，语义路径改为按 id 降序返回最新

- 否决原因：把"新优先"写进检索契约，需要改动 `VectorIndex` 已公开的"并列分按 id 升序"语义（CON-002），影响面外溢到索引层测试；并存期间双倍占容量（CON-001）；且 id 数值大并不代表条目仍在 store 内（CON-003），仍要额外写后复核，收益不足以覆盖代价。

### ALT-003: 用内容哈希作为 id（并入 OPT-001 内容寻址）

- 否决原因：能同时消掉 `exactIndex_` 并天然去重，但 id 变成 prompt 的派生量，需要处理哈希碰撞、把"同 prompt 替换"改写成"同 id 覆盖写"的持久化语义，并重新定义索引的 id 契约；属于设计文档 §5.2 的后续优化项，不是阶段 1 的最小改动。

### ALT-004: `put` 全程持有 `mu_`

- 否决原因：违反 CON-006，会在 `mu_` 内调用 `store_->remove`，而 store 会在锁外回调 `onLeave` 再取 `mu_`，形成锁序反转的死锁风险；也把耗时的 `embed` 串行化，吞吐显著下降。

## 已知限制

- `exactIndex_` 以原始字符串为键，仅对完全相同的 prompt 去重；同义不同字的 query（如"今天天气怎么样"与"今天的天气怎么样"）仍会并存，由语义阈值负责召回，属于预期行为。
- 并发下同 prompt 的两次 `put` 可能都读不到旧 id（CON-007），产生短暂双插；`nextId_` 只能保证 id 递增，不能保证语义路径一定返回较新响应。
