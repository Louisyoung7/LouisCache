## 〇、依赖装配约定

三个运行时依赖由构造函数注入，`Options` 只保留值配置；决策理由与否决方案见 [ADR-0001](file:///home/louis/Documents/VSCodeProjects/CPlusPlusProjects/LouisCache/docs/adr/adr-0001-semantic-cache-dependency-injection.md)。

| 依赖 | 持有方式 | 契约 |
|---|---|---|
| `Embedder` | `shared_ptr<const Embedder>` | 可跨缓存共享 |
| `VectorIndex` | `unique_ptr<VectorIndex>` | 必须独占：id 空间按缓存独立 |
| `EntryStore` | `unique_ptr<Policy<uint64_t, CacheEntry>>` | 必须独占且未设置过回调，由本缓存安装 `onLeave` |

## 一、类的声明

`src/Semantic/SemanticCache.h`（改写现有 stub，唯一实现文件）：

```cpp
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "CacheStats.h"
#include "Embedder.h"
#include "LRU/LruCache.h"   // 仅便捷构造使用；主构造不依赖具体策略
#include "Options.h"
#include "Policy.h"
#include "VectorIndex.h"

namespace louis::cache {

struct GetResult {
    bool hit = false;
    std::string response;
    float score = 0.0f;   // 精确命中记 1.0
    bool exact = false;   // 是否由精确匹配命中
};

// 条目值类型：必须携带 prompt，onLeave 才能反查（并擦除）exactIndex_ 映射。
// 提为顶层而非私有嵌套：外部注入 EntryStore 时需要能命名这个类型。
struct CacheEntry {
    std::string prompt;
    std::string response;
};

class SemanticCache {
   public:
    // 主构造：三个运行时依赖全部由调用方注入。
    // 契约：store 必须是本缓存独占、新建、且未设置过回调的实例；
    //       SemanticCache 在构造期安装 onLeave 并成为唯一写者；此路径下 options.capacity 不生效。
    SemanticCache(Options options,
                  std::shared_ptr<const Embedder> embedder,
                  std::unique_ptr<VectorIndex> vectorIndex,
                  std::unique_ptr<Policy<uint64_t, CacheEntry>> store);

    // 便捷构造：按 options.capacity 新建 LruCache，转调主构造
    SemanticCache(Options options,
                  std::shared_ptr<const Embedder> embedder,
                  std::unique_ptr<VectorIndex> vectorIndex);

    // store_ 的回调捕获 this，禁止拷贝/移动
    SemanticCache(const SemanticCache&) = delete;
    SemanticCache& operator=(const SemanticCache&) = delete;
    SemanticCache(SemanticCache&&) = delete;
    SemanticCache& operator=(SemanticCache&&) = delete;

    GetResult get(const std::string& prompt);
    void put(const std::string& prompt, const std::string& response);
    void remove(const std::string& prompt);

    CacheStats stats() const;   // 快照
    size_t size() const;        // = store_->size()

   private:
    // 唯一清理出口（design §4.1）；由 store_ 在锁外、本线程调用栈内触发
    void onLeave(uint64_t id, const CacheEntry& entry, LeaveReason reason);

    // 以下两个辅助函数要求调用方已持有 mu_
    std::optional<uint64_t> findExactLocked(const std::string& prompt) const;
    void eraseExactIfMatchesLocked(const std::string& prompt, uint64_t id);

    static int clampCapacity(size_t capacity);   // LruCache 的容量是 int

    Options options_;
    std::shared_ptr<const Embedder> embedder_;   // 可跨缓存共享
    std::unique_ptr<VectorIndex> vectorIndex_;   // 独占：id 空间按缓存独立
    std::unique_ptr<Policy<uint64_t, CacheEntry>> store_;  // unique_ptr 即"独占"契约

    mutable std::mutex mu_;   // 只保护下方元数据，绝不跨 store_/vectorIndex_ 调用持有
    std::unordered_map<std::string, uint64_t> exactIndex_;  // 内联，不单建 ExactIndex.h
    std::atomic<uint64_t> nextId_{0};  // key 是 id，与 VectorIndex 唯一关联键（§4.2）
    CacheStats stats_;
};

}  // namespace louis::cache
```

构造函数中的校验与装配：

```cpp
inline int SemanticCache::clampCapacity(size_t capacity) {
    // LruCache 的容量是 int；capacity <= 0 时它会早退且不通知
    constexpr auto kIntMax = static_cast<size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(capacity > kIntMax ? kIntMax : capacity);
}

inline SemanticCache::SemanticCache(Options options, std::shared_ptr<const Embedder> embedder,
                                    std::unique_ptr<VectorIndex> vectorIndex)
    : SemanticCache(options, std::move(embedder), std::move(vectorIndex),
                    std::make_unique<LruCache<uint64_t, CacheEntry>>(
                        clampCapacity(options.capacity))) {}

inline SemanticCache::SemanticCache(Options options, std::shared_ptr<const Embedder> embedder,
                                    std::unique_ptr<VectorIndex> vectorIndex,
                                    std::unique_ptr<Policy<uint64_t, CacheEntry>> store)
    : options_(options), embedder_(std::move(embedder)), vectorIndex_(std::move(vectorIndex)),
      store_(std::move(store)) {
    if (!embedder_) throw std::invalid_argument("embedder is null");
    if (!vectorIndex_) throw std::invalid_argument("vector index is null");
    if (!store_) throw std::invalid_argument("entry store is null");
    // 向量化输出必须能直接进索引，否则每次 add 都会抛异常
    if (embedder_->dim() != vectorIndex_->dim())
        throw std::invalid_argument("embedder dim must match vector index dim");

    store_->setLeaveCallback(
        [this](const uint64_t& id, const CacheEntry& e, LeaveReason r) { onLeave(id, e, r); });
}
```

注入装饰器或自定义策略时直接走主构造，核心类无需感知：

```cpp
auto store = std::make_unique<TtlCache<uint64_t, CacheEntry>>(
    std::make_shared<LruCache<uint64_t, CacheEntry>>(capacity),
    std::chrono::minutes(10));   // TtlCache(shared_ptr<Policy>, Duration)
SemanticCache cache(options, embedder, std::move(index), std::move(store));
```

有两个关键决定：

- **`exactIndex_` 始终维护，`exactIndexEnabled` 只控制 `get` 是否走第 0 步**。否则关闭精确搜索后，`remove(prompt)` 就无法把 prompt 反查成 id。
- **`store_` 用 `unique_ptr` 而不是 `shared_ptr`**：把"同一 store 不能被两个缓存持有（回调只能有一个写者）"这条不变量编码进类型；需要观察 store 的测试可在移入前保留裸指针。

## 二、核心方法时序

> 约定：`[S]` 表示持有 `mu_`，`[空]` 表示未持任何本类锁；`store_` / `vectorIndex_` 各自内部持锁（契约见 §3.1）。

### 2.1 `get(prompt)` — 先精确、后语义

```text
[S] id = exactIndex_.find(prompt)              // 读路径只用 find，不用 operator[]
[空] if (id && store_->get(*id, entry))        // ① 命中即返回；可能触发驱逐回调
[S]     ++stats_.exactHits
        return {hit, entry.response, 1.0f, exact=true}
         // store 未命中 = TOCTOU，视为该 id 未命中，继续往下走（不直接返回）

[空] q = embedder_->embed(prompt)              // ② 无锁、const 纯函数
[空] m = vectorIndex_->search(q, searchK)      // ③ 自有锁；维度不符返回空
     if (m.empty())            [S] ++misses; return miss
     best = m.front()                           // 已按 score 降序，并列按 id 升序
     if (best.score < threshold) [S] ++misses; return miss   // ④ >= 才算命中，score 原样回传

[空] if (!store_->get(best.id, entry))         // ⑤ TOCTOU → miss，不重试
[S]     ++misses; return miss
[S] ++semanticHits; return {hit, entry.response, best.score, exact=false}
```

要点：精确路径**不调用** `embed`；语义路径命中时 `store_->get` 同时完成 recency 刷新（touch）。

### 2.2 `put(prompt, response)` — 覆盖写（upsert），先存储、后索引，并做写后复核

同一 prompt 重复写入是覆盖而非追加，判重只以 `exactIndex_` 为依据（决策与否决方案见 [ADR-0002](file:///home/louis/Documents/VSCodeProjects/CPlusPlusProjects/LouisCache/docs/adr/adr-0002-duplicate-put-upsert.md)）。

```text
[S]  old = exactIndex_.find(prompt)            // 与 get 一致：读路径只用 find
[空] if (old) store_->remove(*old)             // 旧条目经 onLeave(Explicit) 离场：删向量 + 条件删映射

[空] id = nextId_.fetch_add(1)
[S]  ++stats_.puts
[空] vec = embedder_->embed(prompt)            // 先算向量：失败则不落任何存储

[空] store_->put(id, CacheEntry{prompt, response})  // ① 先存储；可能驱逐其他条目 → onLeave
[空] if (!store_->exists(id)) return;          // ② 未驻留（capacity<=0）→ 绝不写索引

[空] vectorIndex_->add(id, vec)                // ③ 后索引
[S]  exactIndex_[prompt] = id                  // ④ 后写精确映射

[空] if (!store_->exists(id)) {                // ⑤ 写后复核：堵住 ②→③ 之间被并发驱逐的窗口
         vectorIndex_->remove(id);             //    幂等
     [S] eraseExactIfMatchesLocked(prompt, id) //    仅当映射仍指向本 id 才删
     }
```

第 ⑤ 步是必要的补偿：如果另一个线程在 `exists` 返回 true 之后、`add` 之前驱逐了该 id，回调执行时向量尚不存在，`add` 会留下一份永远无人清理的向量（`vectorIndex_->size() != store_->size()`）。先 `add` 再复核即可收敛；若驱逐发生在 `add` 之后，则由回调正常清理。

覆盖写的清理复用同一条出口：`store_->remove` 会同步触发 `onLeave(Explicit)`，因此替换、淘汰、过期都只经过 `onLeave` 一处，没有第二套删除逻辑。替换只计入 `stats_.puts`，不计 `evictions`。

两处与当前实现的差异（已知待补，见 §3.2）：`embed` 应先于 `store_->put`，以及第 ⑤ 步写后复核尚未落地。

### 2.3 `remove(prompt)` — 只调 `store_`，不另写删除逻辑

```text
[S]  id = exactIndex_.find(prompt)
[空] if (!id) return
[空] store_->remove(*id)        // 回调 onLeave(Explicit) 统一删向量 + 精确映射
```

`exactIndex_` 的 `erase` 只出现在 `onLeave`（唯一出口）。若 `store_->remove` 因 TOCTOU 空转，说明条目已被驱逐、回调早已清过映射，不会留下残项。

### 2.4 `onLeave(id, entry, reason)` — 唯一清理出口

```text
[空] vectorIndex_->remove(id)               // 先删向量：索引自有锁，刻意不在 mu_ 内
[S]  eraseExactIfMatchesLocked(entry.prompt, id)  // 仅当映射仍指向本条目才删
     reason == Evicted → ++stats_.evictions
     reason == Expired → ++stats_.expirations
```

## 三、并发契约（实现时必须守住）

- **锁序单一**：`mu_` 只包住 `exactIndex_` / `stats_` 的读写；调用 `store_` 任何方法与 `vectorIndex_` 的任何方法前必须先放掉 `mu_`（[§4.4](file:///home/louis/Documents/VSCodeProjects/CPlusPlusProjects/LouisCache/docs/design/03-design-decisions.md#L49-L55)）。否则 `store_->get()` 栈内的回调再取 `mu_` 会自死锁。
- **读路径也会触发回调**：ARC 的 `get` 未命中可能挤出对侧条目，因此 `get` 内不能假设"没有 `put` 就不会有回调"，所有 `mu_` 作用域必须短到只覆盖一次 map/计数器操作。
- **TOCTOU 一律判 miss**，不重试、不回滚重搜；`size()` 在并发下允许瞬时不等，稳定后应相等（验收口径）。
- **回调是叶子**：不得再调用 `store_` 的任何方法；`onLeave` 只碰 `vectorIndex_` 与 `mu_`。

### 3.1 外部依赖契约（`SemanticCache` 成立的前提）

`[空]` 的合法性完全依赖下列**写在接口之外**的契约；替换 `VectorIndex` / `EntryStore` / `Embedder` 实现时必须逐条满足，否则 §二的锁纪律失效。

**VectorIndex**

| 契约 | 违反后果 | 阶段 1 现状 |
|---|---|---|
| `add` / `remove` / `search` / `size` 可并发调用，内部自行同步 | 数据竞争；`[空]` 不再安全 | `BruteForceIndex` 内部 `mutex_` |
| `remove` 幂等，id 不存在时静默返回 | 回调重复删除时抛异常 | 满足 |
| 同一 id 的 `add` 由调用方串行化（阶段 1 单调 id 永不重用） | 后写覆盖；OPT-001 后变成 ABA | 由 `nextId_` 保证 |
| `add` 维度不符抛 `invalid_argument`；`search` 维度不符返回空而非抛 | 构造期 `dim` 校验失去意义 | 满足 |
| `search` 按 score 降序、并列按 id 升序，长度 ≤ k，空索引返回空 | 阈值判断与并列次序不确定 | 满足 |
| 实现不得回调 `SemanticCache`（含紧凑化、异步重建） | 锁序反转、重入死锁 | 无回调 |
| `size()` 只计有效向量，不含已删除/tombstone 条目 | 与 `store_->size()` 的一致性断言误判 | 满足（HNSW 需注意） |

**EntryStore（`Policy<uint64_t, CacheEntry>`）**

| 契约 | 违反后果 | 阶段 1 现状 |
|---|---|---|
| 所有方法可并发调用，内部自行同步 | 数据竞争 | `LruCache` 内部 `mutex_` |
| `notifyLeave` 必须在释放内部锁之后调用，且不得重入 store | `onLeave` 链内自死锁 | `LruCache` 锁内收集、锁外分发 |
| 每个条目恰好触发一次 leave 通知 | 重复删向量或漏删 | 满足 |
| `put` 返回后 `exists(key) == true`（容量 > 0） | §2.2 的驻留判断失效 | 满足；容量 0 时直接丢弃且不通知 |
| `get` 命中时刷新 recency（touch） | 淘汰顺序与预期不符 | 满足 |
| `get` / `exists` 可能因惰性过期触发 leave 通知 | 在 `mu_` 内调用即自死锁 | `LruCache` 不触发；`TtlCache` 会 |
| `get` / `remove` 对不存在的 key 静默返回 | 回调路径抛异常 | 满足 |

**Embedder**

| 契约 | 违反后果 | 阶段 1 现状 |
|---|---|---|
| `embed` 为 const 纯函数且线程安全 | `get` / `put` 并发时数据竞争 | `HashEmbedder` 无状态 |
| `dim()` 固定且等于 `embed` 输出长度 | 构造期校验通过后每次 `add` 仍抛错 | 满足 |
| 同输入同输出（确定性） | 命中不可复现、测试不稳定 | 满足 |

> 这些契约目前只存在于实现里。后续建议同步到 `VectorIndex.h` / `Policy.h` 的接口注释，让新实现者不必读本文也能看到；本文暂作设计侧的事实来源。

### 3.2 已知限制与待修正项（阶段 1）

- 同一 prompt 重复 `put` 已实现为覆盖写（§2.2、ADR-0002）：旧条目经 `store_->remove` 触发 `onLeave(Explicit)` 一并清掉旧向量与映射，不占两份容量。剩余限制在于并发窗口：两个线程同时 put 同一 prompt 时可能都读不到旧 id，出现短暂双插；后写者持有 `exactIndex_` 映射，先写者留在 store 中直到被淘汰，且其 `onLeave` 的条件删除不会误删新映射。严格幂等留给 OPT-001（内容派生 id）。
- `embed` 抛异常、以及 `exists` 与 `add` 之间被并发驱逐这两条路径当前仍会留下残状态：前者在 store 中留一条无向量的条目，后者在索引中留一条无条目的向量。对应测试（`EmbedFailureMustNotLeaveResidentUnindexedEntry`、`EvictionBetweenExistsAndAddLeavesOrphanVector`）仍失败，修法即 §2.2 的"先 embed"与第 ⑤ 步写后复核。
- `options_.exactIndexEnabled` 目前只声明未接线：`get` 仍无条件先查 `exactIndex_`（对应测试 `ExactIndexDisabledSkipsExactPath` 仍失败）。按 §一 的约定，开关只应关闭 `get` 的第 0 步，映射与写入侧照旧维护。
- `size()` 当前返回 `vectorIndex_->size()`，与声明注释的 `= store_->size()` 不一致；这会让 `TtlCache` 注入后失去 `size()` 触发的惰性 purge（对应测试 `TtlPurgeViaSizeCountsExpiration` 仍失败）。验收口径仍是"稳定后两者相等"，实现应改回 `store_->size()`。
- TTL 通过注入 `TtlCache` 组合，核心类与 `Options` 都不感知 TTL。注意注入后 `store_->get()` / `store_->exists()` 可能因惰性过期触发 `onLeave`，因此这两个调用同样不得发生在 `mu_` 作用域内（与 `LruCache` 约束一致）。
- 注入路径下 `options.capacity` 不生效，容量由注入的 store 决定；查看索引是否与存储脱节时，用 `cache.size()` 对比注入前保留的 `vectorIndex` 裸指针（前提是 `size()` 已改为 `store_->size()`）。

## 四、实现顺序

1. `SemanticCache.h` 骨架：`CacheEntry`、`GetResult`、成员类型、主构造（四参）与便捷构造（三参转调）、校验与回调注册。
2. `onLeave` + `findExactLocked` / `eraseExactIfMatchesLocked`。
3. `put`（重复判定与覆盖写、驻留判断、写后复核）。
4. `get`（精确分支 → 语义分支，统计口径分开计）。
5. `remove` / `stats` / `size`，跑一遍现有测试构建确认无编译回归。

## 五、后续优化方向

阶段 1 不做任何一项：当前三结构是为了先把链路与一致性验收跑通。以下按性价比排序，供后续选型。

### 5.1 先分清"冗余"与"必需的第二索引"

| 结构 | 存了什么 | 角色 |
|---|---|---|
| `EntryStore` | `id → {prompt, response}` | 主存储，同时承担淘汰策略与容量统计 |
| `exactIndex_` | `prompt → id` | 第二索引，O(1) 精确命中，跳过 embedding |
| `VectorIndex` | `id → vector` | 第二索引，语义检索 |

按条目计：`prompt` 2 份（entry 值与 map 键）、`id` 3 份（store 键、map 值、index 键）、`response` 1 份、`vector` 1 份。`id` 仅 8 字节可忽略，真正的冗余是 **prompt 的第二份拷贝 + 整张哈希表的节点开销**；而内存大头是向量本身（bge-small 512 维 float ≈ 2 KB/条，`HashEmbedder` 128 维 ≈ 512 B/条）。

比内存更值得关注的是**一致性成本**：三份数据 = 三个一致性点 + 三个锁域，全靠 `onLeave` 一次回调事务化清理。因此优化目标是"消掉一个第二索引"，而不是把三份强行合并成一个结构。

### 5.2 优化项

**OPT-001：用内容哈希当 id，删除 `exactIndex_`（最值得做）**

```text
id = fnv1a64(prompt)          // 稳定、跨进程可复现
exact get : store_->get(id)   // O(1)，仍然不调用 embed
semantic  : index.search → id → store_->get(id)
remove    : store_->remove(fnv1a64(prompt))
onLeave   : vectorIndex_->remove(id)     // 不再需要 prompt 反查
```

- 收益：prompt 从 2 份降到 1 份，exact 哈希表整体消失，一致性点 3 → 2；同一 prompt 重复 `put` 变成同 id 覆盖写，并发下也幂等（不再有"先查后删"的窗口）；`remove(prompt)` 不再依赖 exact 映射，`exactIndexEnabled` 开关也随之失去意义；id 由内容决定，重启后不变，阶段 2 持久化可少存一份 id 映射。
- 代价：64 位碰撞在百万条量级约 `1e-8`，精确命中时用 `entry.prompt == prompt` 校验一次即可降级走语义路径；但"同 id 不同 prompt 互相覆盖"要绝对安全就得把 key 扩到 128 位，`Policy` / `VectorIndex` 的 `uint64_t` 接口需跟着改。另外换哈希算法即换 id 空间，需与 `modelId` 一起写入持久化元数据。
- 时机：放在阶段 2 持久化之前最划算，否则要为单调 id 先落一份映射、随后再废弃。

**OPT-002：向量量化（float16 / int8）**

真实模型下的内存主项，收益比 OPT-001 大一个量级；代价是召回率下降，必须由阶段 3 的阈值—P/R 曲线兜底，因此不适合提前做。

**OPT-003：合并存储与索引（pgvector / Redis 等）**

[§4.8](file:///home/louis/Documents/VSCodeProjects/CPlusPlusProjects/LouisCache/docs/design/03-design-decisions.md) 已分析：能消掉进程内双存储，但外部中间件的淘汰通知异步、不保证投递，会重新打开幽灵命中窗口，属于"换个代价"而非免费优化。

**OPT-004：共享 prompt 字符串（interning / string_view）**

能省掉第二份 prompt，但 `Policy` 会拷贝值、`unordered_map` 会 rehash，引用失效即悬垂，风险高于收益；OPT-001 已从根上解决，不再考虑。

### 5.3 为 OPT-001 预留的前置约束（阶段 1 现在就要守住）

- 不要把 `id` 的单调递增写进验收标准或测试前提；测试应通过 `store_->size()` / `vectorIndex_->size()` 观察一致性，而不是断言 `nextId_` 的取值。
- 持久化元数据除 `modelId` 外，预留"id 生成方式 / 哈希算法版本"，否则换算法后旧索引无法判定不可加载。
- 重复 prompt 现为覆盖写（§2.2），OPT-001 后变为"同 id 覆盖写"并在并发下也幂等；测试只应断言"同 prompt 只留最新一条、不占双份容量"，既不要断言具体 id，也不要把"重复 put 产生两条"固化为预期。
- OPT-001 会改变 [§4.2](file:///home/louis/Documents/VSCodeProjects/CPlusPlusProjects/LouisCache/docs/design/02-architecture.md) 中"id 是唯一关联键"的语义（单调 → 内容派生），落地时需同步修订该节。