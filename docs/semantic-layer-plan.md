# 语义层实现计划（阶段 1 剩余部分）

> 细化 [design.md](design.md) §三、§四、阶段 1 清单中被跳过的实现级决策。TtlCache 已完成（214 用例全绿，`Expired` 已有生产者），本计划覆盖剩余四件交付物。
> 实现顺序（已确认）：**Options/CacheStats → Embedder → VectorIndex → SemanticCache**，自底向上，每步独立测试。
> Workflow 沿用 TtlCache：每步「写头文件 → 审 → 写测试 → 全量验收」。

## 已拍板的决策

| # | 决策 | 理由 |
|---|---|---|
| 1 | **EntryStore 注入式构造**（用户拍板） | 组合现有 6 策略 + TtlCache 自由搭配，贴合「淘汰决策层是护城河」；测试可用容量 1 的 store 做幽灵用例；`Options.capacity` 移除（容量随 store 走） |
| 2 | 重复 put 同 prompt = **更新语义**：复用已有 id | 幂等 put；`store.put(同id)` 覆盖不触发驱逐回调；TTL 刷新、向量 add 覆盖，天然一致 |
| 3 | ExactIndex **内联**进 SemanticCache（`unordered_map<string,uint64_t>`） | design.md 授权"足够薄可内联" |
| 4 | HashEmbedder 用**字符 3-gram feature hashing** | 比"纯串 hash"好：同串必同向量，且近邻串有可控相似度，阈值边界测试可造数据 |
| 5 | BruteForceIndex **接口方法自带锁**（叶子锁） | 被调用线程与 store 回调线程并发访问；无嵌套不破坏锁序 |
| 6 | CacheStats/Options 为纯 struct，无锁 | 由 SemanticCache 的 `mutex_` 保护 |
| 7 | 精确查到但 store 取条目失败（TOCTOU）→ **继续走模糊路径** | 比直接 miss 少漏一次命中机会；模糊路径内的 TOCTOU 仍按 §4.4 视为 miss |

## 文件清单（无 CMake 改动）

- `src/Semantic/Options.h`、`src/Semantic/CacheStats.h` —— 纯数据
- `src/Semantic/Embedder.h` —— `Embedder` 接口 + `HashEmbedder`
- `src/Semantic/VectorIndex.h` —— `VectorIndex` 接口 + `BruteForceIndex`
- `src/Semantic/SemanticCache.h` —— 编排层 + 内联 ExactIndex
- `tests/Embedder_test.cc`、`tests/VectorIndex_test.cc`、`tests/SemanticCache_test.cc`

---

## 第 0 步：Options.h / CacheStats.h

```cpp
struct Options {
  float hitThreshold = 0.85f;     // 语义命中阈值，>= 判定
  size_t searchK = 5;             // VectorIndex.search 返回条数上限
  bool exactIndexEnabled = true;  // false 时 put/get 均不维护 exactIndex_
};

struct CacheStats {
  uint64_t exactHits = 0, semanticHits = 0, misses = 0;
  uint64_t evictions = 0, expirations = 0, puts = 0;
};
```

- 计数点全部在 SemanticCache 内部（get 各分支 / 回调按 reason 分类 / put 收尾），两个 struct 只是数据载体

## 第 1 步：Embedder.h

接口照 design.md 草案：`embed(text) const` / `dim() const` / `modelId() const`。

**HashEmbedder 算法（定死，保证可复现）**：

1. 对 `text` 取所有字符 3-gram（长度 < 3 则整串作为 1 个 gram）
2. 每个 gram 用 FNV-1a 64 位哈希得 `h`：桶 `= h % dim_`，符号 `= (h >> 63) ? -1 : +1`，`vec[bucket] += sign`
3. L2 归一化；模长为 0（空串）返回全零向量

- `dim_` 构造参数，默认 64；`modelId()` 返回 `"hash3-v1:dim=<dim>"`
- 无状态纯函数，`embed` 为 const，天然线程安全
- 空 prompt 合法：全零向量与任何向量点积为 0，永不语义命中（精确路径不受影响）
- 相似度性质：共享 3-gram 越多余弦越大 → "abcdef" vs "abcdeg" 高相似，vs "xyz" 低 → 阈值边界用例可造

## 第 2 步：VectorIndex.h

接口照草案：`add(id, vec)` / `remove(id)` / `search(query, k)` / `size()`；`VectorMatch{id, score}`（相似度口径，越大越相似）。

**BruteForceIndex**：

- 存储 `unordered_map<uint64_t, vector<float>>` + `mutable std::mutex`
- `add`：同 id **覆盖**（配合决策 2）；维度不符 `throw std::invalid_argument`。dim 来源：构造参数（SemanticCache 负责与 `embedder_->dim()` 校验一致）
- `search`：query 维度不符 throw；遍历求点积（注释注明**归一化责任在 Embedder 侧**，索引不重复归一化——§4.6 口径只在适配层内）；`std::partial_sort` 取 top-k 按 score 降序；条目不足 k 返回全部
- `remove`：不存在的 id no-op
- search 复杂度 O(n·d)，MVP 规模无妨

## 第 3 步：SemanticCache.h（分 3a / 3b 两个子步）

### 类型与构造

```cpp
struct Entry { std::string prompt; std::string response; };  // prompt 必须自带：回调只拿得到 (id, Entry, reason)
struct GetResult { bool hit = false; std::string response; float score = 0.0f; bool exact = false; };

SemanticCache(Options options,
              std::shared_ptr<Embedder> embedder,
              std::shared_ptr<VectorIndex> vectorIndex,
              std::shared_ptr<Policy<uint64_t, Entry>> store);
```

- 四参全 null 校验 throw `invalid_argument`；禁拷贝/移动（回调捕获 `this`）
- TTL/淘汰策略由注入的 store 决定（`TtlCache<uint64_t, Entry>` 装饰与否是调用方的事，语义层不感知）

### 锁纪律（§4.4 落地）

- `mutex_` 只保护 `exactIndex_` / `nextId_` / `stats_`
- **持 `mutex_` 时绝不调 `store_`**；锁序单向 `mutex_ → store 内部锁`，store 回调只取 `mutex_` 作叶子
- `vectorIndex_` 自带叶子锁（决策 5），任何线程任何时机可直接调，不持 `mutex_` 调用为默认风格

### put 流程（§4.3 先存储后索引）

```
1. vec = embedder_->embed(prompt)                       // 锁外，最贵操作
2. 锁内：try_emplace(prompt) → 新则 nextId_++ 赋值      // 原子防并发同 prompt 双 id
3. 锁外：store_->put(id, Entry{prompt, response})        // 可能驱逐他人 → 回调清索引
4. 锁外：store_->exists(id) 校验驻留
   ├─ false（容量 0）→ 锁内 exactIndex_.erase(prompt)，跳到 6   // 从未驻留，不写向量
   └─ true → vectorIndex_->add(id, vec)
5. 收尾复查：store_->exists(id) 再查一次
   └─ false（3~4 之间被并发 remove/驱逐）→ vectorIndex_->remove(id) 补偿
      // 推演：回调同步发生于 store.remove 调用栈内（阶段 0 保证），补偿时回调必已完成，无泄漏
6. 锁内：stats_.puts++
```

- `exactIndexEnabled=false` 时跳过 exactIndex_ 读写，其余不变
- **残余竞态（接受，安全侧）**：并发 `remove(p)` 与 `put(p)`——remove 查到 id 后 put 复用同 id 写入，随后 remove 把新条目删掉 → 漏命中；最坏 `add` 与回调 `remove` 交错由步骤 5 补偿封死，`vectorIndex.size() == store.size()` 不变量在任意序列后成立

### get 流程

```
1. exactIndexEnabled → 锁内查 exactIndex_ 得 id → 释放锁
2. store_->get(id, entry) 成功 → exactHits++ → {hit, entry.response, score=1.0, exact=true}
   失败（TOCTOU 已驱逐）→ 降级继续模糊路径（决策 7）
3. vec = embed(prompt)（锁外）→ vectorIndex_->search(vec, searchK)
   → 最高分 score >= hitThreshold → 候选 id
4. store_->get(id, entry)  // touch：天然刷新淘汰策略
   成功 → semanticHits++ → {hit, entry.response, score, exact=false}
   失败 → 视为 miss（§4.4，不取次高分不重试）
5. search 空或 score < 阈值 → misses++ → {hit=false, score=本次最高候选分}
```

- `GetResult.score` 语义：命中 = 匹配分；miss = 本次最高候选分（可观测性，阈值调优需要）

### remove 流程

1. 锁内查 exactIndex_ 得 id；无 → no-op
2. 释放锁 → `store_->remove(id)` → **回调统一清索引**（不手动补刀，一致性只认一个出口，§贯穿原则 4）

### 回调（§4.1 一致性枢纽）

```cpp
store_->setLeaveCallback([this](const uint64_t& id, const Entry& e, LeaveReason r) {
    vectorIndex_->remove(id);     // 不持 mutex_：叶子自带锁，先做重活
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = exactIndex_.find(e.prompt);
        if (it != exactIndex_.end() && it->second == id) exactIndex_.erase(it);
        if (r == LeaveReason::Evicted)      stats_.evictions++;
        if (r == LeaveReason::Expired)      stats_.expirations++;
    }
});
```

- exactIndex_ 条件擦除（id 匹配才删）：防误删并发写入的新映射
- 回调在 store 锁外触发（阶段 0 保证）→ 取 `mutex_` 无死锁；回调内可重入（含 `store_->get`）

### stats() / size()

- `stats()`：锁内拷贝快照返回
- `size()`：`store_->size()`（不持锁）

### 子步拆分

- **3a**：精确路径 + put/remove + 回调 + stats；模糊路径暂返回 miss 占位
- **3b**：接通模糊路径（embed → search → 阈值 → touch → 返回）
- 每子步全量绿后再进下一步

---

## 测试计划

### tests/Embedder_test.cc

1. `DeterministicAcrossInstances` — 两实例同串同向量逐元素相等
2. `DimMatchesConstructor` / `ModelIdContainsDim`
3. `UnitNormForNonEmpty` — 模长 = 1（±1e-6）
4. `EmptyPromptReturnsZeroVector` — 全零且相似度 0
5. `SharedNgramsRaiseSimilarity` — "abcdef" vs "abcdeg" 的余弦显著高于 vs "zyxwvu"（单调性）

### tests/VectorIndex_test.cc

1. `AddSearchRoundtrip` / `AddSameIdOverwrites`
2. `RemoveMakesUnsearchableAndSizeConsistent`
3. `SearchReturnsTopKSortedDescending` / `SearchKGreaterThanSize`
4. `DimMismatchThrows`（add 与 search 两处）
5. `RandomizedAgainstNaiveReference` — 200 随机向量，朴素线性扫描参照实现对拍 top-3
6. `ConcurrentSmoke` — 4 线程 add/remove/search 混跑，size ≤ n

### tests/SemanticCache_test.cc

顶部定义 `CountingEmbedder`（包装 HashEmbedder 计 `embed` 调用次数）+ `ManualClock`（复制 TtlCache_test.cc 模式，零 sleep）：

| # | 用例 | 断言核心 |
|---|---|---|
| 1 | `ExactHitReturnsScore1ExactTrue` | 命中、response 正确 |
| 2 | `ExactHitSkipsEmbedding` | embed 计数 == 0 |
| 3 | `PutSamePromptReusesIdAndUpdates` | size 不变、response 更新、无驱逐 |
| 4 | `RemoveMakesExactMissAndCleansIndexes` | remove 后精确 miss；`vectorIndex.size()==store.size()==0` |
| 5 | `EvictedEntryCleansIndexes` | 容量 1 store：put a → put b → a 驱逐 → query a 不命中、`stats.evictions==1`、size 恒等 |
| 6 | `TtlStoreExpirationNotifiesExpired` | TtlCache(ManualClock) 装饰注入 → 过期 → `stats.expirations==1`、索引已清 |
| 7 | `CapacityZeroStorePutsNoIndexEntry` | 容量 0：`exactIndexEnabled` 下 get miss、size 恒等、无 stats.puts 之外的副作用 |
| 8 | `SizeMatchesStoreAndVectorIndex` | 任意操作序列后三者相等 |
| 9 | `ConcurrentSmoke` | 5 线程 put/get/remove 混跑，结束时 size 恒等、不崩 |
| 10 | `SemanticHitWithinThreshold`（3b） | put("abcdef") → get("abcdeg") 命中、`exact=false`、score 回传且 ≥ 阈值 |
| 11 | `BelowThresholdIsMiss`（3b） | 不相关串 → miss、score 回传最高候选分 |
| 12 | `ThresholdIsInclusive`（3b） | `exactIndexEnabled=false` + get 同串 → score=1.0 命中（验证 `>=`） |
| 13 | `MissScoreIsBestCandidate`（3b） | miss 时 `GetResult.score` 等于朴素计算的最高相似度 |
| 14 | `StatsCountersMatch`（3b） | 各计数与操作序列一一对应 |

## 实施步骤

1. 写 `Options.h` + `CacheStats.h` + `Embedder.h`（三件都很小，一并写）→ 审 → `Embedder_test.cc` → 全量
2. 写 `VectorIndex.h` → 审 → `VectorIndex_test.cc` → 全量
3. 写 `SemanticCache.h`（3a 精确路径）→ 审 → 用例 1-9 → 全量
4. 3b 模糊路径 → 审 → 用例 10-14 → 全量验收（预期 214 + 5 + 8 + 14 = 241 用例）

## 验证

```bash
cmake -S . -B build/Debug -DBUILD_TEST=ON   # 首次新增文件需重新 configure（file(GLOB)）
cmake --build build/Debug -j
ctest --test-dir build/Debug --output-on-failure
```

## 关键风险（已由设计消解）

- **put/add 与 remove 回调竞态**（最大坑）：put 收尾 exists 复查 + 补偿 remove 封死向量泄漏窗口
- **持锁调 store**：所有"锁内查、锁外调"，锁序 `mutex_ → store 锁`，回调取 `mutex_` 为叶子
- **并发同 prompt remove/put**：最坏漏命中，安全侧接受
- **空 prompt**：全零向量，永不语义命中，精确路径不受影响
