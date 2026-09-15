# LouisCache 语义缓存扩展：架构与规划

## 一、项目定位

**LouisCache 现状**：纯头文件 C++ 缓存库，精确匹配 KV 语义，实现了 LRU / LRU-K / LFU-Aging / ARC 及分片版本，线程安全，C++17。

**扩展目标**：语义缓存——面向 AI 基础设施，坐在 LLM 应用和大模型之间。命中时直接返回缓存的回复、跳过 LLM 调用（省 token 成本、降延迟）。注意：**最终用户是 LLM 网关 / Agent 框架 / RAG 应用的开发者，大模型本身是“后端数据源”不是用户**。核心链路中现有库的角色是**淘汰决策层**（决定留谁删谁），存储本身是平凡的 hash map，策略才是护城河。

## 二、总体架构

```
┌────────────────────────────────────────────────────┐
│  调用方：LLM 网关 / Agent 框架 / RAG 应用             │
└───────────────┬────────────────────────────────────┘
                │ get(prompt) / put(prompt, response)
┌───────────────▼────────────────────────────────────┐
│  SemanticCache（编排层）  + Options / CacheStats     │
│                                                    │
│  ① ExactIndex    精确哈希匹配（第0步，免embedding）    │
│  ② Embedder      text→vector（Hash/ONNX/llama.cpp） │
│  ③ VectorIndex   ANN搜索（BruteForce/HNSW）→(id,分)  │
│  ④ 阈值判定       命中策略（自己写的策略点）             │
│  ⑤ EntryStore    id→{prompt, response, 元数据}      │
│     └── 淘汰层：现有 LRU/LFU/ARC + TtlCache 装饰器    │
└────────────────────────────────────────────────────┘
```

**get 数据流**：ExactIndex 哈希命中直接返回 → 未命中则 Embedding → ANN 搜 top-k（k>1）→ 相似度 ≥ 阈值才算命中 → 取回复并 touch 淘汰策略 → 返回 (response, score)。

**put 数据流**：分配 id → 写 EntryStore → 向量入索引；写入可异步化。

**一致性枢纽（全架构最关键的一处设计）**：条目离开缓存的所有路径——容量淘汰、TTL 过期、显式删除——统一走带原因枚举的回调 `LeaveReason { Evicted, Explicit, Expired }`，上层据此同步删除向量索引中的对应向量。没有它就会出现“索引搜到、存储已无”的幽灵命中，这是此类系统最经典的一致性 bug。

**各模块的自研/现成分界**：

| 模块 | 方案 | 自研部分 |
|---|---|---|
| Embedding | ONNX Runtime+tokenizers-cpp 或 llama.cpp（GGUF，bge-small-zh-v1.5 / bge-m3） | 几十行 pooling+归一化适配；HashEmbedder 测试件；`modelId()` 指纹 |
| 向量索引 | hnswlib（头文件库，FetchContent）| 接口抽象 + distance→similarity 方向转换（挡在适配层）|
| 命中判定 | — | 阈值判定、歧义策略，这就是策略层 |
| 淘汰 | 现有库 + TtlCache 装饰器 | 语义感知淘汰（差异化方向）|
| TTL | 惰性删除+定期删除（Redis 式），steady_clock，装饰器模式不改节点 | — |
| 明确不做 | 自研 ANN、线程池/内存池、gRPC 服务端进库、标签失效 | — |

## 三、开发规划

**阶段 0 —— 地基（先行，两个后续模块都在等它）**
- 给所有策略加带 `LeaveReason` 的“条目离开”回调
- `remove()` 提入 `Policy` 接口（目前只是 LruCache 上的非虚函数）
- 单元测试基建（你在 feature/unit_test 分支上的工作）

**阶段 1 —— MVP（保持零依赖）**
- `src/Semantic/`：`Embedder.h`（接口+HashEmbedder）、`VectorIndex.h`（接口+BruteForceIndex）、`SemanticCache.h`、`ExactIndex`、`CacheStats`、`Options`
- `TtlCache` 装饰器
- 用 HashEmbedder+暴力索引完整验证所有编排逻辑，单测覆盖

**阶段 2 —— 真实向量化与生产化**
- ONNX Runtime 路线（CMake 开关 `LOUISCACHE_WITH_ONNX`），真实模型跑通中文场景
- `HnswIndex` 接入：调参（M/ef）、tombstone 紧凑化、索引持久化 + 模型指纹校验
- 异步写入队列、namespace（多实例路由）、`HitVerifier` 接口位（Noop 占位）

**阶段 3 —— 评测与差异化**
- 语义命中率 benchmark：LCQMC/ATEC 同义句数据集，测“阈值-精确率/召回率”曲线（不是传统命中率）
- 延迟/内存 benchmark（对比暴力索引 vs HNSW、各 embedding 延迟分解）
- **语义感知淘汰策略**：优先淘汰向量空间中“冗余”的点、保留覆盖面大的点——这是区别于 GPTCache 等现有方案的研究向护城河
- 服务化 wrapper（`server/` 目录，gRPC/HTTP sidecar 形态，不进核心库）+ 文档

**贯穿性原则**：核心库始终零重依赖（重依赖全部做成 CMake 可选开关）；Stats 和 Options 第一天就带上（阈值调优全靠指标反馈）；每次改造先想清楚向量索引和存储的同步。
