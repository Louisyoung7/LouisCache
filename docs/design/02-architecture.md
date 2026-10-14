# 总体架构

> **分卷**：第 3 卷「总体架构」　|　**文档版本**：v0.1　|　**最近更新**：2026-10-14　|　**对应原文档**：第三节（§3）　|　**关联**：[当前基线](01-baseline.md) · [关键设计决策](03-design-decisions.md)

---

```
┌──────────────────────────────────────────────────────┐
│  调用方：LLM 网关 / Agent 框架 / RAG 应用               │
└──────────────┬───────────────────────────────────────┘
               │ get(prompt) / put(prompt, response)
┌──────────────▼───────────────────────────────────────┐
│  SemanticCache（编排层）  + Options / CacheStats        │
│                                                      │
│  ① ExactIndex    prompt → id  精确匹配（第 0 步，免 embedding） │
│  ② Embedder      text → vector（Hash / ONNX / llama.cpp）     │
│  ③ VectorIndex   ANN 搜索 → [(id, score)]（BruteForce / HNSW） │
│  ④ 命中判定       阈值判定 + 歧义策略（策略点，自研）           │
│  ⑤ EntryStore    id → Entry{prompt, response, 元数据}          │
│       └── 淘汰层：现有 LRU / LFU / ARC + TtlCache 装饰器        │
└──────────────────────────────────────────────────────┘
```

## 3.1 get 数据流

1. `ExactIndex` 命中 → 取 `EntryStore` → **直接返回**（精确命中，score = 1.0，零 embedding 开销）
2. 未命中 → `Embedder.embed(prompt)`
3. `VectorIndex.search(vec, k)`，`k > 1`
4. 取最高分，若 `score >= hitThreshold` → 命中 → `EntryStore` touch（驱动淘汰策略更新）→ 返回 `(response, score)`
5. 否则视为 miss

## 3.2 put 数据流

1. 分配全局唯一 `id`
2. **先** `EntryStore.put(id, Entry{prompt, response})`（可能触发驱逐 → 回调清理索引）
3. **后** `VectorIndex.add(id, vec)` + `ExactIndex[prompt] = id`

顺序不可颠倒，理由见 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引)。

## 3.3 各模块的自研 / 现成分界

| 模块 | 现成方案 | 自研部分 |
|---|---|---|
| Embedding | ONNX Runtime + tokenizers-cpp，或 llama.cpp（GGUF，bge-small-zh-v1.5 / bge-m3） | 几十行 pooling + L2 归一化适配；`HashEmbedder` 测试件；`modelId()` 指纹 |
| 向量索引 | hnswlib（头文件库，FetchContent） | 接口抽象 + `distance → similarity` 方向转换（挡在适配层内） |
| 命中判定 | —— | 阈值判定、歧义策略——**这就是策略层** |
| 淘汰 | 现有库 + `TtlCache` 装饰器 | 语义感知淘汰（阶段 3 差异化方向） |
| TTL | Redis 式惰性删除 + 定期删除，`steady_clock`，装饰器模式不改节点 | —— |

## 3.4 分层职责

| 层 | 是否继承 `Policy` | 理由 |
|---|---|---|
| 现有 6 种策略 | 是 | 精确匹配 KV，接口天然吻合 |
| `TtlCache` 装饰器 | 是 | 装饰另一个 `Policy`，对上层透明 |
| `EntryStore` | 是（`Policy<uint64_t, Entry>`） | 淘汰策略的宿主 |
| `SemanticCache` | **否**（组合而非继承） | `Policy::get` 返回 `bool`，而语义命中需额外返回 `score` 与 `exact` 标志，签名不兼容。硬套接口会把 `score` 塞进 `Value`，污染 `Entry` |

---

**文档导航**　[‹ 上一卷：当前基线](01-baseline.md)　·　[文档索引](../design.md#文档索引)　·　[下一卷：关键设计决策 ›](03-design-decisions.md)
