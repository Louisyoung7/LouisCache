# 分阶段任务清单

> **分卷**：第 5 卷「分阶段任务清单」　|　**文档版本**：v0.1　|　**最近更新**：2026-10-22　|　**对应原文档**：第五节（§5）　|　**关联**：[关键设计决策](03-design-decisions.md) · [贯穿性原则](05-principles.md)

---

## 阶段 0 —— 地基 ✅ 已完成

见 [当前基线（阶段 0）](01-baseline.md)。**验收标准**（已达成）：6 种策略全部接入 `LeaveCallback`；回调锁外分发且可重入；175 个用例通过。

---

## 阶段 1 —— MVP（保持零依赖）

**目标**：用 `HashEmbedder` + `BruteForceIndex` 跑通全部编排逻辑与一致性保证，单测覆盖。此阶段**不追求语义质量**，只追求"链路正确、无幽灵命中、可观测"。

**交付物（文件级）**：

- [x] `src/Semantic/Embedder.h`：`Embedder` 接口 + `HashEmbedder`（确定性、可复现，仅供测试）——**已实现**：默认 `dim = 128`、`ngram = 2`，`modelId()` 形如 `hash-embedder:v1:fnv1a64:ngram=2:dim=128`（实现与算法见 [HashEmbedder 实现与算法详解](../HashEmbedder实现与算法详解.md)）
- [x] `src/Semantic/VectorIndex.h`：`VectorIndex` 接口 + `BruteForceIndex`（内部维护 `id → vector`；维度构造期固定，`add` 维度不符抛 `std::invalid_argument`，`search` 维度不符返回空、`k` 超界截断、并列分按 id 升序）——**已实现**
- [ ] `src/Semantic/ExactIndex.h`：`prompt → id` 精确索引（若足够薄，可内联进 `SemanticCache`）
- [ ] `src/Semantic/SemanticCache.h`：编排层，接口见 [总体架构 §3.5.1](02-architecture.md#351-semanticcache编排层--草案)
- [ ] `src/Semantic/Options.h` / `CacheStats.h`：配置与指标
- [ ] `src/Ttl/TtlCache.h`：`Policy` 装饰器，惰性删除 + 可手动驱动的定期删除，产出 `LeaveReason::Expired`
- [x] `tests/VectorIndex_test.cc`：22 个用例，覆盖构造期维度、`k` 截断、维度校验、并列分次序、幂等 `remove`、并发与 `HashEmbedder` 端到端阈值——**已实现**
- [ ] `tests/SemanticCache_test.cc`、`tests/TtlCache_test.cc`

> 各模块的职责边界与接口见 [总体架构 §3.5](02-architecture.md#35-关键类职责与接口)，此处不再重复。

**验收标准**：

- [ ] 精确命中路径**不调用** `Embedder::embed`（用计数型 mock embedder 断言调用次数为 0）
- [ ] 语义命中路径：query 与已存 prompt 相似度 ≥ 阈值即命中，并在 `GetResult` 中回传 `score`
  - 样本需配合 `HashEmbedder` 默认参数选取：默认 `ngram = 2` 下，短句（6–7 字，差 1 字）余弦 ≈ 0.87、长句（21–22 字，差 1 字）≈ 0.98、无关句 ≈ 0.00；默认阈值 `0.85` 在过短样本上余量有限，参考数值见 [HashEmbedder 实现与算法详解 §5.1](../HashEmbedder实现与算法详解.md)
- [ ] **无幽灵命中**：容量淘汰 / `remove` / TTL 过期后，同义 query 不再命中该条目（逐路径写用例）
- [ ] **无漏清理**：`vectorIndex_->size()` 与 `store_.size()` 在任意操作序列后保持相等
- [ ] 并发 `put` / `get` 下不崩、索引与存储不脱节（沿用阶段 0 的多线程测试写法）
- [ ] `TtlCache` 惰性过期产出 `LeaveReason::Expired`（而非 `Explicit`）
- [ ] `CacheStats` 各计数与预期一致

---

## 阶段 2 —— 真实向量化与生产化

**目标**：把 `HashEmbedder` / `BruteForceIndex` 换成真实实现，并解决容量与持久化问题。

**交付物**：

- [ ] `src/Semantic/OnnxEmbedder.h` + CMake 开关 `LOUISCACHE_WITH_ONNX`：tokenizer-cpp 编码 → session 推理 → mean pooling → L2 归一化；`modelId()` 取模型文件内容哈希
- [ ] 中文场景跑通（bge-small-zh-v1.5），给出典型 query 的 top-k 与分数分布
- [ ] `src/Semantic/HnswIndex.h` + 开关 `LOUISCACHE_WITH_HNSW`：hnswlib 接入，暴露 `M` / `efConstruction` / `efSearch` 调参
- [ ] `HnswIndex::remove` 用 tombstone（`markDelete`）+ 阈值触发紧凑化（重建），避免删除后搜索精度衰减
- [ ] 索引持久化：保存索引 + 元数据（`dim` / `modelId` / 条目数）；加载时校验 `modelId`，不匹配则拒绝加载（向量空间不同不可比较）
- [ ] 异步写入队列：`put` 入队、后台批量 `add`；明确与 `LeaveCallback` 的竞态（未落盘即被驱逐 → 索引无该向量，属安全侧）
- [ ] `Options` 支持 namespace / 多实例路由
- [ ] `HitVerifier` 接口位（`Noop` 占位，为将来"命中后再校验"留口）

> 💡 本阶段涉及的 embedding 模型生态、归一化 / 维度、ANN、modelId 等术语，见 [概念入门 §3–§6](00-primer.md)。

**验收标准**：

- [ ] 真实模型下语义命中率显著优于 `HashEmbedder`，且延迟分解（编码 / 搜索 / 存储）可测
- [ ] 索引删除 + 紧凑化后，召回不出现"删了还能搜到"的残留
- [ ] `modelId` 不匹配时拒绝加载并给出明确错误
- [ ] 关闭所有可选开关时，核心库与阶段 1 行为完全一致

---

## 阶段 3 —— 评测与差异化

**目标**：把"好不好"变成数字，并做出区别于 GPTCache 等现有方案的策略护城河。

**交付物**：

- [ ] 语义命中率 benchmark：基于 LCQMC / ATEC 同义句数据集，测**阈值—精确率/召回率曲线**（不是传统命中率）
- [ ] 延迟与内存 benchmark：暴力索引 vs HNSW 对比；embedding 延迟分解
- [ ] **语义感知淘汰策略**：优先淘汰向量空间中"冗余"的点、保留覆盖面大的点——研究向护城河，需先定义"冗余"的度量
- [ ] `server/` 服务化 wrapper（gRPC / HTTP sidecar 形态，**不进核心库**）+ 使用文档

> 💡 LCQMC / ATEC 是什么？见 [概念入门 §9](00-primer.md#9-benchmark-数据集lcqmc--atec)。

**验收标准**：

- [ ] 能给出"阈值 → P/R"曲线，并据此给出推荐默认阈值
- [ ] 语义感知淘汰在相同命中率下，比 LRU/LFU 基线有可量化的 token 节省或命中率提升

---

**文档导航**　[‹ 上一卷：关键设计决策](03-design-decisions.md)　·　[文档索引](../design.md#文档索引)　·　[下一卷：贯穿性原则 ›](05-principles.md)
