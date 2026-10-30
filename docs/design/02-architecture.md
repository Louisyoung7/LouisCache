# 总体架构

> **分卷**：第 3 卷「总体架构」　|　**文档版本**：v0.1　|　**最近更新**：2026-10-22　|　**对应原文档**：第三节（§3）　|　**关联**：[当前基线](01-baseline.md) · [关键设计决策](03-design-decisions.md)

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

> 💡 图中 ② Embedder、③ VectorIndex 涉及文本向量化与近似检索，见 [概念入门 §1、§4、§5](00-primer.md)。

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
| Embedding | ONNX Runtime + tokenizers-cpp，或 llama.cpp（GGUF，bge-small-zh-v1.5 / bge-m3） | 几十行 pooling + L2 归一化适配；`HashEmbedder` 测试件（确定性字节 n-gram 特征哈希，[实现与算法](../components/HashEmbedder实现与算法详解.md)）；`modelId()` 指纹 |
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

## 3.5 关键类职责与接口

各模块的职责边界与接口草稿如下。**已实现**者直接给出落点文件；未实现者以「草案」标注，签名以实现时为准。

### 3.5.1 `SemanticCache`（编排层）— 草案

**它是什么**：整个库对外的**唯一入口**，也就是图中的「编排层」。它**自己不实现任何算法**——不向量化、不检索、不淘汰——只负责把几个**策略无关的构件**按固定顺序串成一次完整操作：`Embedder`（向量化）、`VectorIndex`（检索）、`EntryStore`（存储 + 淘汰）、`ExactIndex`（精确匹配）。因此它通过**组合**持有这些构件（见 [§3.4](#34-分层职责)），而非继承 `Policy`。

**它在语义缓存里做什么**（三个对外方法各自编排一条固定路径）：

- **`get`**：对应 [§3.1](#31-get-数据流) 的完整数据流。先查 `ExactIndex`（第 0 步，命中即**免去 embedding**，`score = 1.0`、`exact = true`）；未命中再 `Embedder.embed(prompt)` → `VectorIndex.search(vec, searchK)` → 取返回结果中的最高分与 `hitThreshold` 比较（第 ④ 步命中判定）→ 命中则对 `EntryStore` 做一次 touch（驱动淘汰策略更新）并把 `score` 回填进结果。整条路径对调用方只暴露一个 `GetResult`。
- **`put`**：对应 [§3.2](#32-put-数据流)。分配全局唯一 `id`，**先**写 `EntryStore`（这一步可能触发驱逐，由回调清理索引），**确认条目真的驻留后**再写 `VectorIndex` 与 `ExactIndex`。顺序不可颠倒（见 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引)）。
- **`remove`**：只调 `EntryStore.remove`，由 `LeaveCallback`（`LeaveReason::Explicit`）统一清理索引——`SemanticCache` **不再另写删除逻辑**，索引的删除只有一个出口。

**职责要点与约束**：

- **全局唯一 `id` 分配**：`put` 时递增分配 `id`，它是 `EntryStore` 与 `VectorIndex` 之间**唯一的关联键**（向量索引返回的是 `id`，见 [§4.2](03-design-decisions.md#42-entrystore-用-id-而非-prompt-作-key)）；`stats_` 同步记录 `puts`。
- **注册唯一清理出口**：构造时把回调交给 `EntryStore`，回调内**同步**清理 `VectorIndex` 与 `ExactIndex`（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。必须容忍**读路径触发的驱逐**：ARC 的 `get` 未命中也可能挤出对侧条目并发回调，故 `get` 也要能正确处理（不能假设「只有 `put` 才触发驱逐」）。
- **`Entry` 必须自带 `prompt`**：回调只拿得到 `(id, Entry, reason)`，`SemanticCache` 凭 `Entry.prompt` 反查并 `exactIndex_.erase(prompt)`——这也是 `value` 不能只存 `response`、必须连 `prompt` 一起存的原因（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。
- **锁序纪律**：自持互斥锁**只保护自身元数据**（`exactIndex_`、`id` 分配、`stats_`）；**调用 `EntryStore` / `VectorIndex` 的任何方法前必须先释放语义层锁**，否则回调会在 `store.get()` 的调用栈内再取同一把非递归 `std::mutex`，形成**自死锁**（见 [§4.4](03-design-decisions.md#44-锁序持有语义层锁时不得调用-entrystore)）。锁方向单一：语义层锁 → 存储锁，回调作为叶子只取语义层锁，无环。
- **容忍 TOCTOU**：`get` 路径上「查到 `id` → 放锁 → `store.get(id)`」之间条目可能已被驱逐——一律**视为 miss**（安全侧），不重试（见 [§4.4](03-design-decisions.md#44-锁序持有语义层锁时不得调用-entrystore)）。
- **`capacity <= 0` 时不得写索引**：`EntryStore` 对 `capacity <= 0` 会早退且**不通知**，故 `put` 必须依据其反馈判断条目是否真的驻留，未驻留则**不写** `VectorIndex` / `ExactIndex`（见 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引)）。
- **统计口径**：`stats_` 至少区分 `exactHits` / `semanticHits` / `misses` / `puts` / `evictions` / `expirations`（见 [§3.5.7](#357-options--cachestats--已实现)），它们是阈值调优的反馈输入（见 [§4.5](03-design-decisions.md#45-阈值判定--策略层)）。

```cpp
struct GetResult {
    bool hit = false;
    std::string response;
    float score = 0.0f;  // 精确命中记 1.0
    bool exact = false;  // 是否由精确匹配命中
};

class SemanticCache {
   public:
    SemanticCache(Options options,
                  std::shared_ptr<Embedder> embedder,
                  std::shared_ptr<VectorIndex> vectorIndex);

    GetResult get(const std::string& prompt);
    void put(const std::string& prompt, const std::string& response);
    void remove(const std::string& prompt);

    CacheStats stats() const;
    size_t size() const;

   private:
    struct Entry {  // 必须自带 prompt：回调只能拿到 (id, Entry, reason)，凭此反查 ExactIndex
        std::string prompt;
        std::string response;
    };

    Options options_;
    std::shared_ptr<Embedder> embedder_;
    std::shared_ptr<VectorIndex> vectorIndex_;
    std::unique_ptr<Policy<uint64_t, Entry>> store_;  // 阶段 1 为 EntryStore，外层可套 TtlCache
    ExactIndex exactIndex_;

    std::mutex mu_;                    // 只保护下方元数据
    std::atomic<uint64_t> nextId_{0};  // 全局唯一 id 分配
    CacheStats stats_;
};
```

### 3.5.2 `Embedder` / `HashEmbedder` — 已实现

**它是什么**：**文本 → 向量**的抽象。语义缓存的全部相似度比较都建立在向量之上，而 `Embedder` 就是这层「把任意 prompt 变成能算相似度的定长向量」的转换器。接口极薄，三个方法：`embed(text)`、`dim()`、`modelId()`。

**它在语义缓存里做什么**：对应 `get` 数据流的第 ② 步（见 [§3.1](#31-get-数据流)）——**只有精确索引未命中才触发**，是「语义命中」相对于「精确命中」多付的那一笔开销（精确命中走第 0 步、零 embedding）。`put` 时同样要对新 prompt 调一次 `embed`，把向量交给 `VectorIndex.add`（见 [§3.2](#32-put-数据流)）。

**为什么单独抽成接口**：编排层只依赖 `Embedder` 抽象，于是「零依赖跑通」用 `HashEmbedder`、生产用 ONNX / llama.cpp，都只在 CMake 可选开关处切换，上层编排不改（见 [§4.7](03-design-decisions.md#47-零依赖原则与可选开关)）。

**职责要点**：

- **输出契约**：定长（`dim()` 个分量）、**L2 归一化**的向量。归一化后内积等价于余弦相似度，`VectorIndex` 可直接用点积打分，且相似度与文本长度解耦、与真实模型口径一致（见 [§4.6](03-design-decisions.md#46-distance--similarity-转换挡在适配层)）。
- **`modelId()` 是向量空间指纹**：把影响向量空间的一切参数（算法、版本、`dim`、`ngram`…）编进字符串。持久化索引加载时**必须校验指纹一致**——不同指纹的向量不可比较；任何改变向量空间语义的改动（换算法 / 改常量）都要 **bump 版本号**，否则会被误判为同一空间，算出的相似度全部无意义。
- **线程安全**：`embed()` 声明为 `const`、实现无内部可变状态，故可被多线程并发只读调用，与 `VectorIndex::search` 的 const 只读并发对齐（见 [§3.5.3](#353-vectorindex--bruteforceindex--已实现)）。
- **真实后端**：ONNX Runtime + tokenizers-cpp，或 llama.cpp（GGUF，bge-small-zh-v1.5 / bge-m3）；自研部分仅几十行 pooling + L2 归一化适配（见 [§3.3](#33-各模块的自研--现成分界)）。

```cpp
class Embedder {
   public:
    virtual ~Embedder() = default;
    virtual std::vector<float> embed(const std::string& text) const = 0;  // L2 归一化
    virtual size_t dim() const = 0;
    // 模型指纹：向量空间标识，持久化索引加载时校验，不同指纹不可比较
    virtual std::string modelId() const = 0;
};
```

**`HashEmbedder`（测试替身，已实现）**：确定性、可复现的**假向量生成器**，仅用于测试编排逻辑，**不代表语义质量**。

- **算法（特征哈希 / hashing trick）**：滑动 n-gram → FNV-1a 64 位哈希 → 过 MurmurHash3 终结器 `mix` 做低位白化 → `% dim` 定桶、取符号位定正负 → 累加 → L2 归一化。算法细节见 [HashEmbedder 实现与算法详解](../components/HashEmbedder实现与算法详解.md)。
- **带符号累加**：使向量均值 ≈ 0，cosine 近似「共享 n-gram 占比」——无关文本相互抵消而非恒正，贴合真实 embedding 的口径。
- **确定性来源**：固定 FNV 偏移基数作初值、常量冻结，保证跨进程可复现；`dim` / `ngram` 参与 `modelId`，**改了它们就等于换了向量空间**。
- **边界兜底**：长度不足 `ngram`（含空串）时退化为「整串单特征」，保证至少一个非零分量；累加恰好抵消为 0 时按整串哈希单桶重建，避免 cosine 出现 `0/0 = NaN`。
- 接口与实现见 [Embedder.h](../../src/Semantic/Embedder.h#L9-L37)。

### 3.5.3 `VectorIndex` / `BruteForceIndex` — 已实现

**它是什么**：一份「向量索引」——持有所有已缓存条目各自的**向量**（key 是条目 `id`，与 `EntryStore` 的 key 一一对应），并能对任意 query 向量**快速找出最相近的若干条**，即「近似最近邻搜索（ANN）」（见 [概念入门 §4](00-primer.md#4-ann-近似最近邻暴力搜索-vs-hnsw)）。

**它在语义缓存里做什么**：对应 `get` 数据流的第 ③ 步（见 [§3.1](#31-get-数据流)）。当精确索引未命中时，编排层先 `Embedder.embed(prompt)` 得到 query 向量，再交给 `VectorIndex.search` 拿回一组 `(id, score)`；编排层取其中最高分与 `hitThreshold` 比较，据此完成第 ④ 步「命中判定」。所以 `VectorIndex` 只负责**「谁最像」**，不负责**「算不算命中」**——后者是策略层的事（见 [§4.5](03-design-decisions.md#45-阈值判定--策略层)）。

**为什么单独抽成接口**：编排层只依赖 `VectorIndex` 抽象，阶段 1 用零依赖的 `BruteForceIndex` 打通链路、阶段 2 换 `HnswIndex` 都不必改上层（见 [§4.7](03-design-decisions.md#47-零依赖原则与可选开关)）；同时把 `distance → similarity` 的口径转换**封在适配层内**，不泄漏到阈值判定（见 [§4.6](03-design-decisions.md#46-distance--similarity-转换挡在适配层)）。

**职责要点**：

- **写入 / 删除与 `EntryStore` 同步**：`add(id, vec)` 在 `put` 的「后索引」阶段调用；`remove(id)` 由 `LeaveCallback` 触发，确保条目被淘汰 / TTL 过期 / 显式删除后其向量一并消失——这是**杜绝幽灵命中**的关键一环（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。因此应恒有 `VectorIndex::size() == EntryStore::size()`（验收项，见 [04-roadmap](04-roadmap.md#阶段-1--mvp保持零依赖)）。
- **`search` 返回 top-k 且按 `score` 降序**：`k` 取自 `Options.searchK`。取 `k > 1` 而非只取最近邻，是为了给**歧义策略**（多个近邻分数接近时的取舍）留口（见 [§3.1](#31-get-数据流) 第 3 步）。
- **`score` 语义统一**：对外一律是**相似度**（越大越相似，与阈值同向）。`BruteForceIndex` 在向量已 L2 归一化的前提下用内积（等价于余弦）；`HnswIndex` 内部多用距离，需在适配层换算成正向相似度，两套口径不得混用。
- **维度构造期固定**：`VectorIndex(dim)` 让每个实现绑定一个向量空间，`dim == 0` 构造即抛 `std::invalid_argument`。`add` 维度不符抛异常（写入侧 fail-loud，且不污染索引）；`search` 维度不符（含空 query）返回空（读路径落到 miss，安全侧）；`k` 截断到 `min(k, size())`，不足 `k` 条不补位。维度同时是阶段 2 持久化元数据的必填项。
- **结果可复现**：并列分按 `id` 升序作次级键，避免 `std::sort` 对相等元素次序未定义。
- **查询是 `const`**：`search` 为只读，不修改索引状态，便于上层并发只读；维度校验在加锁前完成（`dim_` 不可变）。
- **后端可替换**：`BruteForceIndex` 自持 `id → vector` 做**线性扫描**（O(N)、结果精确、实现简单、便于测试）；阶段 2 的 `HnswIndex` 用图结构走捷径（快，但可能漏掉个别最优解），其 `remove` 需 tombstone + 紧凑化（见 [04-roadmap](04-roadmap.md#阶段-2--真实向量化与生产化)）。

**能否换成外部中间件（PostgreSQL / pgvector 等）？** 接口上**可以**——`VectorIndex` 是纯抽象，pgvector / Milvus / Qdrant / Redis-VSS 都能实现同一组方法，故存储与 ANN 是可替换的「轮子」（见 [§4.7](03-design-decisions.md#47-零依赖原则与可选开关)）。但「能换」不等于「原地替换」，代价与边界约束见 [§4.8](03-design-decisions.md#48-存储--向量索引可外部化清理边界与决策留在进程内)：**清理的同步语义、淘汰决策与命中判定仍留在进程内**，且不应只外置向量（需与 `response` 同库以保事务性）。因此外部后端只应作为 `server/` 侧可选适配器，不进核心库。

**接口**（已实现；阶段 2 会在此基础上加持久化 `save` / `load` + `modelId` 校验，见 [04-roadmap](04-roadmap.md#阶段-2--真实向量化与生产化)）：

```cpp
struct VectorMatch {
    uint64_t id;
    float score;  // 相似度，越大越相似；输入已 L2 归一化时即点积
};

class VectorIndex {
   public:
    virtual ~VectorIndex() = default;
    size_t dim() const noexcept;  // 构造期固定，之后不可变

    // 维度必须等于 dim()，否则抛 std::invalid_argument
    virtual void add(uint64_t id, const std::vector<float>& vec) = 0;
    // 幂等：id 不存在时为空操作
    virtual void remove(uint64_t id) = 0;
    // k 为返回条数上限（超过 size() 自动截断）；结果按 score 降序，并列分按 id 升序
    // query 维度必须等于 dim()，否则返回空
    virtual std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const = 0;
    virtual size_t size() const = 0;

   protected:
    explicit VectorIndex(size_t dim);  // dim == 0 抛 std::invalid_argument

   private:
    const size_t dim_;
};

class BruteForceIndex : public VectorIndex {
   public:
    explicit BruteForceIndex(size_t dim);

    void add(uint64_t id, const std::vector<float>& vec) override;
    void remove(uint64_t id) override;
    std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const override;
    size_t size() const override;

   private:
    std::unordered_map<uint64_t, std::vector<float>> vectors_;
    mutable std::mutex mutex_;  // search/size 为 const，故 mutable
};
```

> 接口落点见 [VectorIndex.h](../../src/Semantic/VectorIndex.h#L17-L52)；22 个契约用例见 [VectorIndex_test.cc](../../tests/VectorIndex_test.cc)。

### 3.5.4 `ExactIndex` — 草案

**它是什么**：`prompt → id` 的**精确映射**（本质就是一张哈希表），方向与 `EntryStore` 恰好相反——`EntryStore` 回答「这个 `id` 的内容是什么」，`ExactIndex` 回答「这个 prompt 对应哪个 `id`」。两者在 [§4.2](03-design-decisions.md#42-entrystore-用-id-而非-prompt-作-key) 里是配对设计的。

**它在语义缓存里做什么**：支撑 `get` 的**第 0 步**（见 [§3.1](#31-get-数据流)）——prompt 完全一致时**直接命中、免掉 embedding**，`score = 1.0`、`exact = true`。写入发生在 `put` 的后索引阶段（见 [§3.2](#32-put-数据流)），删除发生在 `LeaveCallback` 内（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。

**为什么需要它**：若不做精确索引、全部走 embedding + 向量搜索，则阈值取 1.0 时精确命中也会被当作语义命中，但**每次精确命中都要付一次 embedding**——与「第 0 步免 embedding」的设计目标冲突（这是 [§4.2](03-design-decisions.md#42-entrystore-用-id-而非-prompt-作-key) 明确放弃的方案）。

**职责要点**：

- **足够薄，可内联**：底层就是一个 `std::unordered_map<std::string, uint64_t>`；若不想单独抽象，可把这张表直接放进 `SemanticCache`，本节接口即它的边界。
- **写路径用 `operator[]`**：`exactIndex_[prompt] = id` 一句完成「新增或覆盖」，无需再为写维护一个 `add`（`operator[]` 天然返回引用，直接赋值即可）。
- **读路径用 `find` 而非 `operator[]`**：标准 `unordered_map::operator[]` 在 key 不存在时会**默认插入** `id = 0`，读路径用它就会凭空多出一个指向 `0` 的条目、污染索引；且 `operator[]` **非 const**，不能用于并发的只读 `get`。故读走 `find` 返回 `std::optional`，天然表达「未命中」。
- **`erase` 只能由 `LeaveCallback` 调用**：删除的唯一出口是回调（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)），`ExactIndex` 自身不主动删——以此保证「表里存在的 prompt 必有驻留条目」。
- **一致性边界**：`exactIndex_` 与 `id` 分配、`stats_` 同属「语义层锁保护的元数据」（见 [§4.4](03-design-decisions.md#44-锁序持有语义层锁时不得调用-entrystore)），其与 `EntryStore` 的最终一致由回调 + 写入顺序共同保证（见 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引)）。

```cpp
class ExactIndex {
   public:
    // 写：新增或覆盖 prompt → id。用法：exactIndex_[prompt] = id
    uint64_t& operator[](const std::string& prompt);

    // 读：不存在返回 nullopt。刻意不提供 const operator[]——
    // 标准 unordered_map::operator[] 会默认插入 id=0 且非 const，读路径不可用
    std::optional<uint64_t> find(const std::string& prompt) const;

    void erase(const std::string& prompt);  // 由 LeaveCallback 调用（唯一删除出口）
    size_t size() const;

   private:
    std::unordered_map<std::string, uint64_t> map_;
};
```

### 3.5.5 `EntryStore`（复用阶段 0 的 `Policy<uint64_t, Entry>`）— 已实现

**它是什么**：**条目存储 + 淘汰策略宿主**。它并非新写的容器——而是阶段 0 那组 `Policy<Key, Value>`（LRU / LRU-K / LFU / ARC / 分片等）以 `Key = uint64_t`、`Value = Entry` 实例化后的产物：**存储结构直接复用现成策略**，语义层只额外挂两点需求——「唯一清理出口」与「写入是否驻留的反馈」。

**它在语义缓存里做什么**：`get` 的第 ⑤ 步（见 [§3.1](#31-get-数据流)）——拿到 `id` 后在此取回 `Entry{prompt, response}`；同时是 `put` 的**第一步**（见 [§3.2](#32-put-数据流)）——先落这里、确认驻留，再写索引。

**职责要点**：

- **key 用 `id`、value 自带 `prompt`**（见 [§4.2](03-design-decisions.md#42-entrystore-用-id-而非-prompt-作-key)）：向量索引返回的是 `id`，用 `id` 作主键可一次查表取回条目；`Entry.prompt` 则让回调能反查并清理 `ExactIndex`。
- **`setLeaveCallback` = 唯一清理出口**（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。回调经基类 `notifyLeave` 在**锁外**触发、可重入、内部流转静默——这是阶段 0 就设计好的性质，正好满足「条目真正离开时做一次精确清理」。
- **`capacity <= 0` 早退且不通知**：`put` 后必须判断条目是否**真的驻留**（用 `exists(key)` 或返回值）；若未驻留则**不得**写索引（见 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引)）。
- **两套 `get` 的取舍**：`bool get(const Key&, Value&)`（传出参数、返回是否命中）与 `Value get(const Key&)`（未命中返回默认构造值）。语义层应用**前者**——后者无法区分「命中但值恰等于默认值」与「未命中」，会误判命中。
- **`exists` 是 peek 语义**：不刷新 recency、不触发淘汰、过期条目返回 `false`（`const` 接口）。用于 `put` 后的驻留判断时不会污染 LRU/LFU 顺序，与会驱动淘汰的 `get` 区分开。
- **`capacity()` 语义按策略而异**：ARC 返回 T1+T2 主缓存总容量，Sharded 返回分片总量——均为**用户设定值**而非实时条数。
- **回调是叶子**：`notifyLeave` 为 `const`（只读回调、不改缓存状态），且按 [§4.4](03-design-decisions.md#44-锁序持有语义层锁时不得调用-entrystore) 在存储锁外触发、只在内部取语义层锁——因此语义层回调内**不可**再调用 `EntryStore` 的任何方法。
- **本身即 `Policy`，可再装饰**：外层可直接套 `TtlCache`（见 [§3.5.6](#356-ttlcache-装饰器--已实现)）而语义层无感。

```cpp
template <typename Key, typename Value>
class Policy {  // 阶段 0 已实现；EntryStore = Policy<uint64_t, Entry>
   public:
    using LeaveCallback = std::function<void(const Key&, const Value&, LeaveReason)>;

    virtual void put(Key key, Value value) = 0;
    virtual bool get(const Key& key, Value& value) = 0;  // 传出参数，返回是否命中
    virtual Value get(const Key& key) = 0;               // 命中返回值，否则默认构造
    virtual void remove(const Key& key) = 0;
    void setLeaveCallback(LeaveCallback cb);             // 非虚：唯一清理出口

    virtual size_t size() const = 0;
    virtual bool exists(const Key& key) const = 0;       // peek：不刷 recency / 不触发淘汰
    virtual size_t capacity() const = 0;                 // 用户设定容量（语义按策略而异）
   protected:
    void notifyLeave(const Key& k, const Value& v, LeaveReason r) const;  // 锁外触发
};
```

> 接口落点见 [Policy.h](../../src/Policy.h#L7-L47)。

### 3.5.6 `TtlCache` 装饰器 — 已实现

**它是什么**：一个**装饰器（Decorator）**——与内层实现同一个 `Policy` 接口，套在任意 `Policy` 外层叠加 TTL，对上层与内层**双向透明**（见 [§3.3](#33-各模块的自研--现成分界)）。

**它在语义缓存里做什么**：给条目加「过期时间」。它**不改动内层节点**，而是自持一张 `expiryMap_` 记录每个 key 的到期时刻；过期产出 `LeaveReason::Expired`，经同一回调出口并入 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口) 的统一清理。

**职责要点**：

- **两种过期回收**：**惰性删除**——`get` / `exists` 顺手清该 key、`size()` 顺手清全部过期项；**定期删除**——`purgeExpired()` 由上层按需驱动全量清扫，返回本次清掉的过期项数。
- **回调缓冲 + 统一重分类**：内层回调先被缓冲进 `pendingInnerLeaves_`（由 `bufferMutex_` 保护，作为**叶子锁**），再由外层 `drainAndClassifyLocked` 一次性吐给语义层。关键目的是**改写 `reason`**：某条目即便在内层恰好被驱逐、但确已过期，也要上报为 `Expired` 而非 `Evicted`。
- **分类优先级**：① 本次主动清出的 `expiredVictims` 先报 `Expired`；② 缓冲中的 `Explicit` 一律报 `Explicit`（用户显式删除优先于过期）；③ `Evicted` 先查「是否已到期」——到期则改报 `Expired`，否则原样。
- **不改内层 + `const` 接口**：`size()` / `exists()` 是 `const` 却需在锁内 purge / drain，故 `expiryMap_` / `mutex_` / `pendingInnerLeaves_` 声明为 `mutable`（互斥锁仍是唯一保护手段）。
- **禁止拷贝与移动**：内层 `inner_` 的回调闭包捕获了 `this`——拷贝会使 `this` 路由混乱，移动会使闭包内指针悬垂。
- **构造约束**：`inner` 为空抛出 `std::invalid_argument`；`Clock` 是模板参数（默认 `std::chrono::steady_clock`），可注入假时钟做确定性测试。
- **写入时同步维护**：`put` 时 `expiryMap_[key] = now + ttl_`；若因内层 `capacity == 0` 未真正驻留，则同步 `expiryMap_.erase(key)`——**不触发任何回调**（与 [§4.3](03-design-decisions.md#43-写入顺序先存储后索引) 的「不驻留不写索引」一致）。

```cpp
template <typename Key, typename Value, typename Clock = std::chrono::steady_clock>
class TtlCache : public Policy<Key, Value> {
   public:
    TtlCache(std::shared_ptr<Policy<Key, Value>> inner, Duration ttl);  // inner 为空抛异常

    void put(Key key, Value value) override;   // 记 expiryMap_ + 透传内层
    bool get(const Key& key, Value& value) override;   // 惰性清该 key
    Value get(const Key& key) override;
    void remove(const Key& key) override;

    size_t size() const override;              // 惰性清全部过期项
    bool exists(const Key& key) const override;  // 惰性清该 key
    size_t capacity() const override;          // 透传内层

    size_t purgeExpired();                     // 定期删除：返回清掉的过期项数

   private:
    std::shared_ptr<Policy<Key, Value>> inner_;
    Duration ttl_;
    // size()/exists() 为 const 却需在锁内 purge/drain，故 mutable
    mutable std::unordered_map<Key, TimePoint> expiryMap_;  // 装饰器自持过期表
    mutable std::mutex mutex_;
    mutable std::vector<LeaveRecord> pendingInnerLeaves_;   // 内层回调缓冲
    mutable std::mutex bufferMutex_;                        // 叶子锁
};
```

> 接口落点见 [TtlCache.h](../../src/Ttl/TtlCache.h#L13-L137)。

### 3.5.7 `Options` / `CacheStats` — 已实现

**它是什么**：一对**配置 / 可观测性**契约——`Options` 是**输入端**（怎么调），`CacheStats` 是**反馈端**（调得怎么样）。两者从第一天就要存在，否则 `hitThreshold` 无从调优（见 [§4.5](03-design-decisions.md#45-阈值判定--策略层)）。

**职责要点**：

- **`Options` 四项**：`capacity`（`EntryStore` 容量，经 `Policy::capacity` 下发）/ `hitThreshold`（语义命中阈值，策略层的**操作点**）/ `searchK`（ANN 取 top-k，取 `k > 1` 给歧义策略留口，见 [§3.5.3](#353-vectorindex--bruteforceindex--已实现)）/ `exactQueryEnabled`（是否启用第 0 步精确匹配，只作用于 `get` 读路径）。
- **`CacheStats` 六个口径**：`exactHits` / `semanticHits` **分开计数**——第 0 步命中与语义命中成本不同，混在一起就看不出精确索引是否值回票价；命中率 = `(exactHits + semanticHits) / (exactHits + semanticHits + misses)`，`misses` 才是读侧的第三个落点，`puts` 是写侧的成功写入次数（含覆盖写），不参与命中率分母；`evictions` / `expirations` 来自 `LeaveCallback`、按 `LeaveReason` 分类计数（见 [§4.1](03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)）。
- **写者单一**：`stats_` 由语义层锁保护，只在 `SemanticCache` 内维护（见 [§4.4](03-design-decisions.md#44-锁序持有语义层锁时不得调用-entrystore)）；`options_` 是构造期快照、运行期只读，因此不参与锁序；`stats()` 对外返回**快照**。

```cpp
struct Options {
    size_t capacity = 1000;         // EntryStore 容量
    float hitThreshold = 0.85f;     // 语义命中阈值
    size_t searchK = 5;             // ANN 搜索 K 个最近邻
    bool exactQueryEnabled = true;  // 是否启用精确查询（仅 get 读路径）
};

struct CacheStats {
    uint64_t exactHits = 0;     // 精确命中次数
    uint64_t semanticHits = 0;  // 语义命中次数
    uint64_t misses = 0;        // 未命中次数
    uint64_t evictions = 0;     // LeaveReason::Evicted
    uint64_t expirations = 0;   // LeaveReason::Expired
    uint64_t puts = 0;          // 成功写入次数（含覆盖写）
};
```

> 接口落点见 [Options.h](../../src/Semantic/Options.h#L6-L11) 与 [CacheStats.h](../../src/Semantic/CacheStats.h#L6-L13)。

---

**文档导航**　[‹ 上一卷：当前基线](01-baseline.md)　·　[文档索引](../design.md#文档索引)　·　[下一卷：关键设计决策 ›](03-design-decisions.md)
