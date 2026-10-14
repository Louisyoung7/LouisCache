# 关键设计决策

> **分卷**：第 4 卷「关键设计决策」　|　**文档版本**：v0.1　|　**最近更新**：2026-10-14　|　**对应原文档**：第四节（§4）　|　**关联**：[总体架构](02-architecture.md) · [分阶段任务清单](04-roadmap.md)

---

## 4.1 一致性枢纽：`LeaveCallback` 是唯一清理出口

**问题**：这类系统最经典的 bug 是"索引搜到、存储已无"的**幽灵命中**——条目被淘汰/TTL 过期/显式删除后，向量索引里仍留着它的向量，`get` 会搜到一个不存在的 id。

**决策**：所有离开路径（`Evicted` / `Explicit` / `Expired`）统一走阶段 0 已有的 `LeaveCallback`，回调内**同时**清理 `ExactIndex` 与 `VectorIndex`。

**为什么可行**：阶段 0 已保证回调在锁外触发、可重入、内部流转静默——正好满足"在条目真正离开时做一次精确清理"的要求。这不是巧合，阶段 0 就是为此设计的。

**落点**：

```cpp
// EntryStore 的 value 必须自带 prompt，回调只拿得到 (id, Entry, reason)
store_.setLeaveCallback([this](const uint64_t& id, const Entry& e, LeaveReason reason) {
    vectorIndex_->remove(id);        // 删向量
    exactIndex_.erase(e.prompt);     // 删精确索引
    // 统计 evictions / expired（按 reason 分类计数）
});
```

**注意**：ARC 的 `get` 未命中也可能产生 `Evicted`（命中幽灵缓存会重划分容量、挤出对侧条目），因此语义层必须能在**读路径**上容忍并正确处理回调，不能假设"只有 put 才触发驱逐"。

## 4.2 `EntryStore` 用 `id` 而非 `prompt` 作 key

**决策**：`EntryStore` 是 `Policy<uint64_t, Entry>`，另设 `ExactIndex: prompt → id`。

**理由**：
- 向量索引天然返回 `id`，用 `id` 作主键可在搜索结果上**一次查表**拿到条目，无需反向映射。
- 若反过来用 `prompt` 作 key，则需维护 `id → prompt` 反向表，且 `Entry` 会被复制进策略节点与向量索引两处。
- `Entry` 内含 `prompt`，回调即可反查并清理 `ExactIndex`，不需要额外结构。

**放弃的方案**：不做 `ExactIndex`、全部走 embedding + 向量搜索（阈值取 1.0 时精确命中也会被判为语义命中）。省一个 map，但每次精确命中都要付 embedding 开销——与"第 0 步免 embedding"的设计目标冲突。

> 💡 什么是 embedding、「第 0 步」如何免掉它？见 [概念入门 §1](00-primer.md#1-embedding文本向量化)。

## 4.3 写入顺序：先存储、后索引

**决策**：`put` 必须先 `EntryStore.put`（可能驱逐、回调清索引），再 `VectorIndex.add` + `ExactIndex` 写。

**理由**：取反会产生**幽灵命中**窗口——索引已含新向量但存储尚无该条目，此时并发 `get` 会搜到一个取不出内容的 id。按"先存储后索引"的顺序，最坏情况只是短暂的**漏命中**（索引里还差一个向量，语义搜索搜不到，但精确匹配仍能命中），安全性高一档。

**推论**：若 `EntryStore` 因容量为 0 直接丢弃（现有实现会对 `capacity <= 0` 早退且不通知），则**不得**写入索引——需要让 `EntryStore` 能反馈"条目是否真的驻留"。

## 4.4 锁序：持有语义层锁时不得调用 `EntryStore`

**问题**：`EntryStore` 的回调可能在同一线程内、于 `store.get()` / `store.put()` 的调用栈中触发（锁外，但仍在调用栈内）。若此时调用方还持有语义层的锁，回调再去取同一把锁即**自死锁**（非递归 `std::mutex`）。

**决策**：语义层锁只保护自身元数据（`exactIndex_`、id 分配、`stats_`）；**调用 `EntryStore` 的任何方法前必须释放语义层锁**。锁的获取顺序单一方向：语义层锁 → 存储锁，回调只作为叶子取语义层锁，无环。

**衍生的 TOCTOU**：`get` 路径上"查到 `id` → 释放锁 → `store.get(id)`"之间条目可能已被驱逐。处理策略：**视为 miss**（安全侧），不重试。同理，`ExactIndex` 与 `EntryStore` 的最终一致性由回调保证。

## 4.5 阈值判定 = 策略层

`hitThreshold` 不是"越接近 1 越好"的常量，而是**精确率 / 召回率曲线的操作点**：调高→精确但漏命中多，调低→命中多但可能返回不相关回复。阶段 1 先用可配置常量打通链路，阶段 3 用数据集把这条曲线测出来。`CacheStats` 与 `Options` 从第一天就要有，否则阈值无从调优。

> 💡 精确率 / 召回率是什么？见 [概念入门 §7](00-primer.md#7-阈值与精确率--召回率)。

## 4.6 `distance → similarity` 转换挡在适配层

hnswlib 与 BruteForce 内部用距离（越小越近），语义层用相似度（越大越相似）。方向转换与归一化口径**只在 `VectorIndex` 适配层内做一次**，不泄漏到阈值判定逻辑，避免两套口径混用导致的阈值失灵。

> 💡 相似度与距离为何方向相反？见 [概念入门 §2](00-primer.md#2-向量相似度与距离方向)。

## 4.7 零依赖原则与可选开关

核心库保持**零重依赖**：`HashEmbedder` + `BruteForceIndex` 足以跑通全部编排逻辑。重依赖（ONNX Runtime、hnswlib）全部做成 CMake 可选开关（如 `LOUISCACHE_WITH_ONNX`、`LOUISCACHE_WITH_HNSW`），默认关闭。好处是：测试在无 GPU、无模型、无网络的环境下也能全绿。

---

**文档导航**　[‹ 上一卷：总体架构](02-architecture.md)　·　[文档索引](../design.md#文档索引)　·　[下一卷：分阶段任务清单 ›](04-roadmap.md)
