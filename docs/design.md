# LouisCache 语义缓存扩展：架构与实施规划

> 本文档回答三个问题：**做什么**（目标与边界）、**怎么做**（架构与关键决策）、**下一步做什么**（分阶段任务清单与验收标准）。
> 进度基线：**阶段 0（地基）已完成**，详见第二节。

---

## 一、项目定位与边界

### 1.1 现状

LouisCache 目前是**纯头文件 C++17 缓存库**，只做**精确匹配**的 KV 缓存，实现了 LRU / LRU-K / LFU / ARC 及分片版本，线程安全、无第三方依赖。

### 1.2 扩展目标

在现有库之上构建**语义缓存**：以 `get(prompt) / put(prompt, response)` 为接口，命中时直接返回历史回复、跳过 LLM 调用，省 token 成本、降延迟。

### 1.3 使用者与边界

- **最终用户**：LLM 网关 / Agent 框架 / RAG 应用的开发者。
- **不是用户**：大模型本身——它是"后端数据源"，不在调用链上。
- **核心资产判断**：存储本身是平凡的 hash map，**淘汰决策 + 命中判定才是护城河**。因此现有库在架构中的角色是**淘汰决策层**，语义层是编排层。

### 1.4 明确不做

自研 ANN 算法、通用线程池/内存池、把 gRPC/HTTP 服务端塞进核心库、标签/多字段失效、分布式一致性。以上若做，放在 `server/` 或独立项目。

---

## 二、当前基线（阶段 0 已完成）

阶段 0 的目标是给语义层铺好地基，两条主线：**条目离开的统一出口**、**测试基建**。

| 能力 | 状态 | 落点 |
|---|---|---|
| `Policy` 抽象接口：`put` / `get`×2 / `remove` / `size` | ✅ | [Policy.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/Policy.h#L7-L42) |
| `LeaveReason { Evicted, Explicit, Expired }` + 回调注册 | ✅ | [Policy.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/Policy.h#L5-L38) |
| `remove()` 提升为虚接口（原先仅是 `LruCache` 上的非虚函数） | ✅ | [Policy.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/Policy.h#L26) |
| 6 种策略全部接入回调：LRU / LRU-K / Sharded LRU / LFU / Sharded LFU / ARC | ✅ | `src/LRU`、`src/LFU`、`src/ARC` |
| **锁外分发**回调（回调内可安全重入缓存） | ✅ | 各实现均"锁内收集、锁外 `notifyLeave`" |
| **内部流转静默**（复合结构内部迁移不通知） | ✅ | [LruKCache.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/LRU/LruKCache.h)、[ArcCache.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/ARC/ArcCache.h) |
| 单元测试基建（Google Test + Conan + CTest） | ✅ | [tests/](file:///home/louis/Documents/VSCodeProjects/LouisCache/tests)，6 文件 **175 用例**全绿 |

### 2.1 地基已具备、语义层可直接依赖的三条语义

这是阶段 0 真正的产出，阶段 1 的编排逻辑必须建立在其上：

1. **所有离开路径统一走 `LeaveCallback(key, value, reason)`**：容量淘汰 → `Evicted`、`remove()` → `Explicit`、过期 → `Expired`。语义层只需注册一次回调，即可在同一处同步清理外部索引。
2. **回调在缓存内部锁全部释放后触发**：回调内可安全重入缓存的任意方法，且能观察到条目离开后的最终状态。
3. **复合结构内部流转对外不可见**：`LruKCache` 的历史队列晋升、`ArcCache` 的 T1→T2 迁移与幽灵缓存挤出/命中均静默，只有条目真正离开缓存才通知。

### 2.2 阶段 0 遗留、需在阶段 1 顺手补齐的缺口

| 缺口 | 影响 | 处理 |
|---|---|---|
| `Expired` 枚举已定义但**无生产者** | TTL 能力缺失 | 阶段 1 实现 `TtlCache` 装饰器 |
| `Policy` 无 `contains()` / `capacity()` / `clear()` | `contains` 仅在 `LruCache` 上；语义层要判断驻留需绕路 | 按需补入 `Policy`，避免为将来预留 |
| 无 `Stats` / `Options` | 阈值调优无数据反馈 | 阶段 1 随 `SemanticCache` 一起设计 |
| 回调 value 为大对象时按 `const&` 传递，但 `put` 为传值语义 | 语义层 `Entry` 含 prompt+response，拷贝成本需留意 | 阶段 1 实测后再决定是否加移动重载 |

---

## 三、总体架构

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

### 3.1 get 数据流

1. `ExactIndex` 命中 → 取 `EntryStore` → **直接返回**（精确命中，score = 1.0，零 embedding 开销）
2. 未命中 → `Embedder.embed(prompt)`
3. `VectorIndex.search(vec, k)`，`k > 1`
4. 取最高分，若 `score >= hitThreshold` → 命中 → `EntryStore` touch（驱动淘汰策略更新）→ 返回 `(response, score)`
5. 否则视为 miss

### 3.2 put 数据流

1. 分配全局唯一 `id`
2. **先** `EntryStore.put(id, Entry{prompt, response})`（可能触发驱逐 → 回调清理索引）
3. **后** `VectorIndex.add(id, vec)` + `ExactIndex[prompt] = id`

顺序不可颠倒，理由见 [§4.3](#43-写入顺序先存储后索引)。

### 3.3 各模块的自研 / 现成分界

| 模块 | 现成方案 | 自研部分 |
|---|---|---|
| Embedding | ONNX Runtime + tokenizers-cpp，或 llama.cpp（GGUF，bge-small-zh-v1.5 / bge-m3） | 几十行 pooling + L2 归一化适配；`HashEmbedder` 测试件；`modelId()` 指纹 |
| 向量索引 | hnswlib（头文件库，FetchContent） | 接口抽象 + `distance → similarity` 方向转换（挡在适配层内） |
| 命中判定 | —— | 阈值判定、歧义策略——**这就是策略层** |
| 淘汰 | 现有库 + `TtlCache` 装饰器 | 语义感知淘汰（阶段 3 差异化方向） |
| TTL | Redis 式惰性删除 + 定期删除，`steady_clock`，装饰器模式不改节点 | —— |

### 3.4 分层职责

| 层 | 是否继承 `Policy` | 理由 |
|---|---|---|
| 现有 6 种策略 | 是 | 精确匹配 KV，接口天然吻合 |
| `TtlCache` 装饰器 | 是 | 装饰另一个 `Policy`，对上层透明 |
| `EntryStore` | 是（`Policy<uint64_t, Entry>`） | 淘汰策略的宿主 |
| `SemanticCache` | **否**（组合而非继承） | `Policy::get` 返回 `bool`，而语义命中需额外返回 `score` 与 `exact` 标志，签名不兼容。硬套接口会把 `score` 塞进 `Value`，污染 `Entry` |

---

## 四、关键设计决策

### 4.1 一致性枢纽：`LeaveCallback` 是唯一清理出口

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

### 4.2 `EntryStore` 用 `id` 而非 `prompt` 作 key

**决策**：`EntryStore` 是 `Policy<uint64_t, Entry>`，另设 `ExactIndex: prompt → id`。

**理由**：
- 向量索引天然返回 `id`，用 `id` 作主键可在搜索结果上**一次查表**拿到条目，无需反向映射。
- 若反过来用 `prompt` 作 key，则需维护 `id → prompt` 反向表，且 `Entry` 会被复制进策略节点与向量索引两处。
- `Entry` 内含 `prompt`，回调即可反查并清理 `ExactIndex`，不需要额外结构。

**放弃的方案**：不做 `ExactIndex`、全部走 embedding + 向量搜索（阈值取 1.0 时精确命中也会被判为语义命中）。省一个 map，但每次精确命中都要付 embedding 开销——与"第 0 步免 embedding"的设计目标冲突。

### 4.3 写入顺序：先存储、后索引

**决策**：`put` 必须先 `EntryStore.put`（可能驱逐、回调清索引），再 `VectorIndex.add` + `ExactIndex` 写。

**理由**：取反会产生**幽灵命中**窗口——索引已含新向量但存储尚无该条目，此时并发 `get` 会搜到一个取不出内容的 id。按"先存储后索引"的顺序，最坏情况只是短暂的**漏命中**（索引里还差一个向量，语义搜索搜不到，但精确匹配仍能命中），安全性高一档。

**推论**：若 `EntryStore` 因容量为 0 直接丢弃（现有实现会对 `capacity <= 0` 早退且不通知），则**不得**写入索引——需要让 `EntryStore` 能反馈"条目是否真的驻留"。

### 4.4 锁序：持有语义层锁时不得调用 `EntryStore`

**问题**：`EntryStore` 的回调可能在同一线程内、于 `store.get()` / `store.put()` 的调用栈中触发（锁外，但仍在调用栈内）。若此时调用方还持有语义层的锁，回调再去取同一把锁即**自死锁**（非递归 `std::mutex`）。

**决策**：语义层锁只保护自身元数据（`exactIndex_`、id 分配、`stats_`）；**调用 `EntryStore` 的任何方法前必须释放语义层锁**。锁的获取顺序单一方向：语义层锁 → 存储锁，回调只作为叶子取语义层锁，无环。

**衍生的 TOCTOU**：`get` 路径上"查到 `id` → 释放锁 → `store.get(id)`"之间条目可能已被驱逐。处理策略：**视为 miss**（安全侧），不重试。同理，`ExactIndex` 与 `EntryStore` 的最终一致性由回调保证。

### 4.5 阈值判定 = 策略层

`hitThreshold` 不是"越接近 1 越好"的常量，而是**精确率 / 召回率曲线的操作点**：调高→精确但漏命中多，调低→命中多但可能返回不相关回复。阶段 1 先用可配置常量打通链路，阶段 3 用数据集把这条曲线测出来。`CacheStats` 与 `Options` 从第一天就要有，否则阈值无从调优。

### 4.6 `distance → similarity` 转换挡在适配层

hnswlib 与 BruteForce 内部用距离（越小越近），语义层用相似度（越大越相似）。方向转换与归一化口径**只在 `VectorIndex` 适配层内做一次**，不泄漏到阈值判定逻辑，避免两套口径混用导致的阈值失灵。

### 4.7 零依赖原则与可选开关

核心库保持**零重依赖**：`HashEmbedder` + `BruteForceIndex` 足以跑通全部编排逻辑。重依赖（ONNX Runtime、hnswlib）全部做成 CMake 可选开关（如 `LOUISCACHE_WITH_ONNX`、`LOUISCACHE_WITH_HNSW`），默认关闭。好处是：测试在无 GPU、无模型、无网络的环境下也能全绿。

---

## 五、分阶段任务清单

### 阶段 0 —— 地基 ✅ 已完成

见第二节。**验收标准**（已达成）：6 种策略全部接入 `LeaveCallback`；回调锁外分发且可重入；175 个用例通过。

---

### 阶段 1 —— MVP（保持零依赖）

**目标**：用 `HashEmbedder` + `BruteForceIndex` 跑通全部编排逻辑与一致性保证，单测覆盖。此阶段**不追求语义质量**，只追求"链路正确、无幽灵命中、可观测"。

**交付物（文件级）**：

- [ ] `src/Semantic/Embedder.h`：`Embedder` 接口 + `HashEmbedder`（确定性、可复现，仅供测试）
- [ ] `src/Semantic/VectorIndex.h`：`VectorIndex` 接口 + `BruteForceIndex`（内部维护 `id → vector`，支持 `remove`）
- [ ] `src/Semantic/ExactIndex.h`：`prompt → id` 精确索引（若足够薄，可内联进 `SemanticCache`）
- [ ] `src/Semantic/SemanticCache.h`：编排层，见下方接口草案
- [ ] `src/Semantic/Options.h` / `CacheStats.h`：配置与指标
- [ ] `src/Ttl/TtlCache.h`：`Policy` 装饰器，惰性删除 + 可手动驱动的定期删除，产出 `LeaveReason::Expired`
- [ ] `tests/SemanticCache_test.cc`、`tests/TtlCache_test.cc`

**接口草案**（具体签名以实现时为准）：

```cpp
// ---- Embedder.h ----
class Embedder {
 public:
  virtual ~Embedder() = default;
  virtual std::vector<float> embed(const std::string& text) const = 0;
  virtual size_t dim() const = 0;
  // 模型指纹：向量空间标识。持久化索引加载时校验，不同指纹不可比较
  virtual std::string modelId() const = 0;
};

// ---- VectorIndex.h ----
struct VectorMatch {
  uint64_t id;
  float score;  // 相似度，越大越相似
};

class VectorIndex {
 public:
  virtual ~VectorIndex() = default;
  virtual void add(uint64_t id, const std::vector<float>& vec) = 0;
  virtual void remove(uint64_t id) = 0;
  // k 为返回条数上限；返回结果按 score 降序
  virtual std::vector<VectorMatch> search(const std::vector<float>& query, size_t k) const = 0;
  virtual size_t size() const = 0;
};

// ---- Options.h / CacheStats.h ----
struct Options {
  size_t capacity = 1000;         // EntryStore 容量
  float hitThreshold = 0.85f;     // 语义命中阈值
  size_t searchK = 5;             // ANN 返回条数
  bool exactIndexEnabled = true;  // 是否启用第 0 步精确匹配
};

struct CacheStats {
  uint64_t exactHits = 0;    // 精确命中
  uint64_t semanticHits = 0; // 语义命中
  uint64_t misses = 0;
  uint64_t evictions = 0;    // LeaveReason::Evicted
  uint64_t expirations = 0;  // LeaveReason::Expired
  uint64_t puts = 0;
};

// ---- SemanticCache.h ----
struct GetResult {
  bool hit = false;
  std::string response;
  float score = 0.0f;   // 精确命中记 1.0
  bool exact = false;   // 是否由精确匹配命中
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
  struct Entry {  // 必须自带 prompt：回调只能拿到 (id, Entry, reason)
    std::string prompt;
    std::string response;
  };
  // ... exactIndex_ / vectorIndex_ / embedder_ / store_(Policy<uint64_t, Entry>)
};
```

**验收标准**：

- [ ] 精确命中路径**不调用** `Embedder::embed`（用计数型 mock embedder 断言调用次数为 0）
- [ ] 语义命中路径：query 与已存 prompt 相似度 ≥ 阈值即命中，并在 `GetResult` 中回传 `score`
- [ ] **无幽灵命中**：容量淘汰 / `remove` / TTL 过期后，同义 query 不再命中该条目（逐路径写用例）
- [ ] **无漏清理**：`vectorIndex_->size()` 与 `store_.size()` 在任意操作序列后保持相等
- [ ] 并发 `put` / `get` 下不崩、索引与存储不脱节（沿用阶段 0 的多线程测试写法）
- [ ] `TtlCache` 惰性过期产出 `LeaveReason::Expired`（而非 `Explicit`）
- [ ] `CacheStats` 各计数与预期一致

---

### 阶段 2 —— 真实向量化与生产化

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

**验收标准**：

- [ ] 真实模型下语义命中率显著优于 `HashEmbedder`，且延迟分解（编码 / 搜索 / 存储）可测
- [ ] 索引删除 + 紧凑化后，召回不出现"删了还能搜到"的残留
- [ ] `modelId` 不匹配时拒绝加载并给出明确错误
- [ ] 关闭所有可选开关时，核心库与阶段 1 行为完全一致

---

### 阶段 3 —— 评测与差异化

**目标**：把"好不好"变成数字，并做出区别于 GPTCache 等现有方案的策略护城河。

**交付物**：

- [ ] 语义命中率 benchmark：基于 LCQMC / ATEC 同义句数据集，测**阈值—精确率/召回率曲线**（不是传统命中率）
- [ ] 延迟与内存 benchmark：暴力索引 vs HNSW 对比；embedding 延迟分解
- [ ] **语义感知淘汰策略**：优先淘汰向量空间中"冗余"的点、保留覆盖面大的点——研究向护城河，需先定义"冗余"的度量
- [ ] `server/` 服务化 wrapper（gRPC / HTTP sidecar 形态，**不进核心库**）+ 使用文档

**验收标准**：

- [ ] 能给出"阈值 → P/R"曲线，并据此给出推荐默认阈值
- [ ] 语义感知淘汰在相同命中率下，比 LRU/LFU 基线有可量化的 token 节省或命中率提升

---

## 六、贯穿性原则

1. **核心库零重依赖**：重依赖全部做成 CMake 可选开关，默认关闭。
2. **Stats 与 Options 第一天就带上**：阈值调优全靠指标反馈。
3. **先想同步再动手**：每次改造先回答"向量索引和存储怎么保持一致"。
4. **一致性只认一个出口**：条目离开索引清理只在 `LeaveCallback` 一处实现，不新增旁路。
5. **可测试性优先**：可复现的 `HashEmbedder` + 暴力索引是全部编排逻辑的测试基座，真实模型只是可替换件。
