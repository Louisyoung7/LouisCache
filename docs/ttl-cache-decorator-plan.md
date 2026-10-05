# TtlCache 装饰器实现计划

## Context

LouisCache 阶段 1（语义缓存 MVP）的第一个交付物。`LeaveReason::Expired` 目前无生产者，语义层"TTL 过期后清理索引"的一致性保证（design.md §4.1）依赖它先存在。TtlCache 只依赖 `Policy` 接口，零依赖可独立测试，完成后 `SemanticCache` 直接组合 `TtlCache<Policy<uint64_t, Entry>>` 作为 EntryStore。

**已确认的设计决策**（用户拍板）：
1. 全局统一 TTL（构造时传入，所有条目共享）
2. 固定过期：`put` 时定格 `now + ttl`，get/exists 不续期；重新 put 重置过期时间
3. 模板时钟参数 `Clock = std::chrono::steady_clock`，测试注入 ManualClock，零 sleep

## 新增文件（无 CMake 改动）

- `src/Ttl/TtlCache.h` — INTERFACE 库已把 `src/` 作为 include 目录，无需注册
- `tests/TtlCache_test.cc` — tests 用 `file(GLOB *.cc)`，自动纳入（需重新 configure）

## 类设计

```cpp
template <typename Key, typename Value,
          typename Clock = std::chrono::steady_clock>
class TtlCache : public Policy<Key, Value> {
    std::shared_ptr<Policy<Key, Value>> inner_;
    typename Clock::duration ttl_;
    std::unordered_map<Key, typename Clock::time_point> expiryMap_;  // 装饰器自持过期表
    mutable std::mutex mutex_;
    std::vector</*记录*/> pendingInnerLeaves_;  // inner 回调缓冲
    std::mutex bufferMutex_;                    // 叶子锁
};
```

构造：`TtlCache(std::shared_ptr<Policy<Key, Value>> inner, typename Clock::duration ttl)`，null inner 抛 `std::invalid_argument`；禁用拷贝/移动（回调捕获 `this`，同 [ShardedLruCache.h](file:///home/louis/Documents/VSCodeProjects/LouisCache/src/LRU/ShardedLruCache.h) 的理由）。头文件注明"不得共享裸 inner 指针"。

**公开 API**（除 `purgeExpired` 外全部 `override`）：

| 方法 | 语义 |
|---|---|
| `put(Key, Value)` | 先记 `expiryMap_[key] = now + ttl`，再 `inner->put`；容量 0 时 inner 未存则事后擦除自己的 expiryMap_ 条目（无回调——条目从未驻留） |
| `get(const Key&, Value&)` / `Value get(const Key&)` | 过期 → 惰性清除单个条目并返回 miss；未过期 → 委托 inner |
| `remove(const Key&)` | 删自己的 expiryMap_ 条目 + 委托 inner.remove，向外报 `Explicit` |
| `size()` | **先全量 purge 再委托**（保证语义层 `vectorIndex.size() == store.size()` 不变量） |
| `exists(const Key&) const` | 过期条目惰性清除并返回 false（硬约束）；未过期委托 inner.exists |
| `capacity()` | 委托 inner |
| `size_t purgeExpired()` | 手动驱动的定期删除，返回清除数量（供测试/统计） |

## 核心机制：inner 回调缓冲 + 分类转发

**问题**：装饰器调用 `inner->remove()` 做过期清除时，inner 的回调报 `Explicit`，但对外必须报 `Expired`；且回调需要的 value 只有 inner 持有。

**方案**：构造时给 inner 安装回调，handler 只做一件事——加 `bufferMutex_` 把 `(key, value, reason)` 追加进缓冲。**handler 绝不取 `mutex_`**（装饰器方法持 `mutex_` 调 inner，handler 在该调用栈内触发，非递归锁会自死锁）。锁序单向：`mutex_ → inner 内部锁 → bufferMutex_`（叶子）。

每个公开方法统一骨架（**Fix：通知必须在 mutex_ 释放后**，保住阶段 0"回调可重入"的保证）：

```cpp
void put(Key key, Value value) override {
    auto now = Clock::now();                        // 每个公开方法取一次快照，口径一致
    std::vector<LeaveRecord> outward;
    std::unordered_map<Key, Value> expiredVictims;  // 局部！判定集合 + 预取值缓存二合一
    {
        std::lock_guard<std::mutex> lock(mutex_);
        expiryMap_[key] = now + ttl_;               // 先记，供后续分类查 zombie
        inner_->put(key, std::move(value));         // 可能缓冲 Evicted
        outward = drainAndClassifyLocked(now, expiredVictims);
        // 容量 0 兜底：inner 未真正驻留则擦除自己的表项，无回调（条目从未驻留）
        if (!inner_->exists(key)) expiryMap_.erase(key);
    }
    for (auto& e : outward) this->notifyLeave(e.key, e.value, e.reason);
}
```

**命名修正：`expiredVictims`（不是 `selfRemoved`）**。它只装"装饰器因过期而清除的 key"，不装用户 `remove()`——后者是用户行为，必须报 `Explicit`。一个 map 承担两个角色：key 在 map 中 = "这条缓冲 Explicit 是我过期清除触发的，抑制并改报 `Expired`"；map 的 value = remove 前预取的 value（回答"值放哪儿"）。做**局部**变量不做成员，避免跨调用/跨线程脏状态。

**判定靠调用点上下文，不靠缓冲内容**：缓冲事件只有 `(key, value, reason)`，不知道自己被谁触发。谁触发由调用点决定——用户 remove 走空 `expiredVictims` 的 drain（Explicit 原样转发）；purge helper 清除时先预取 value 放进 `expiredVictims` 再 `inner->remove`（缓冲的 Explicit 被 drain 抑制改报 Expired）；inner 容量淘汰不经装饰器调用，drain 按缓冲 reason 分类。

**私有 helper 分两层**：

```cpp
// 核心：只看单条；只读 expiryMap_ 不擦除（擦除统一留给 drain）
// 顺序：查 expiryMap_ → 未过期返回 false
//       → inner_->get(key, v) 预取（即将离开，刷新 recency 无妨）
//       → expiredVictims.emplace(key, std::move(v))
//       → inner_->remove(key)（触发缓冲 Explicit）→ 返回 true
bool purgeKeyIfExpiredLocked(const Key& key, typename Clock::time_point now,
                             std::unordered_map<Key, Value>& expiredVictims);

// 批量：全表扫描，循环调上面那个；size()/purgeExpired() 用
void purgeExpiredLocked(typename Clock::time_point now,
                        std::unordered_map<Key, Value>& expiredVictims);

// 统一排空 + 分类 + 擦表（expiryMap_ 的所有擦除都集中在这里）
// 返回值语义：结果 push 进函数内部局部 vector 后按值 return，无需传出参数。
// 注意 C++17 强制拷贝消除只覆盖"返回纯右值（临时对象）"；返回具名局部走
// NRVO + 移动兜底，但 vector 移动是 O(1)，按值返回即免费
// expiredVictims 用非 const &：drain 是其最后消费者，push 时 std::move(v)，
// 避免把大 Value（LLM 回复字符串）拷贝进 LeaveRecord
std::vector<LeaveRecord> drainAndClassifyLocked(
    typename Clock::time_point now,
    std::unordered_map<Key, Value>& expiredVictims);
```

**`drainAndClassifyLocked` 两段结构（顺序敏感）**：

```
1) 先消化 expiredVictims：push {k, std::move(v), Expired}，erase expiryMap_[k]
   —— 不依赖缓冲事件是否到达，更稳
2) 再排空缓冲（取 bufferMutex_ 叶子锁，逐条 move 出来），逐条：
     if k 在 expiredVictims 中 → 跳过（第 1 段已处理）
     else if r == Explicit → push {k, v, Explicit}，erase expiryMap_[k]
                              （用户显式 remove，即使条目恰好已过期也报 Explicit）
     else /* Evicted */ → 先读 expiryMap_[k] 是否已过期，再 erase，然后 push
                          {k, v, 已过期(zombie) ? Expired : Evicted}
```

**三个易错点**：

1. `Evicted` 分支必须**先读过期状态、再 erase**，顺序反了永远查不到
2. 全表扫描时**不得在迭代 expiryMap_ 的同时 erase**——`purgeKeyIfExpiredLocked` 只读不擦，扫描期间只有 `inner_->remove` 往缓冲追加、不动 expiryMap_，迭代器安全（更保守可先把受害者 key 收集进 vector 再处理）
3. `inner_->get(key, v)` 返回 bool 要防御性判断；false 说明表与 inner 脱节（仅可能来自外部共享裸 inner，已禁用），跳过即可

**各方法对 helper 的使用**：

- `size()` / `purgeExpired()`：先 `purgeExpiredLocked`（真正从 inner 删掉过期项）再委托 inner——`size()` 因此**不会**把已过期未删项算进去，且保证 `store_.size() == 外部索引 size()` 不变量
- `get` / `exists`：走 `purgeKeyIfExpiredLocked` 惰性清除单条（命中的 key 同样进 `expiredVictims`），随后各自委托 inner；被清除的 key 返回 miss
- `remove()`：走空 `expiredVictims` 的 drain，缓冲 `Explicit` 原样转发（即使条目恰好已过期）

## 测试计划（tests/TtlCache_test.cc）

复用 [LruCache_test.cc](file:///home/louis/Documents/VSCodeProjects/LouisCache/tests/LruCache_test.cc#L284-L309) 的 `LeaveRecorder` 模式（改为适配 TtlCache），顶部定义 ManualClock（`Clock::now()` 静态接口 + `advance()`）：

1. `TtlCacheTest.PutGetHitWithinTtl` — ManualClock 基础读写
2. `TtlCacheTest.GetReturnsMissAfterExpiry` — 两个 get 重载都返回 miss
3. `TtlCacheTest.ExistsReturnsFalseAfterExpiry`
4. `TtlCacheTest.GetDoesNotRenewExpiry` — 固定过期验证
5. `TtlCacheTest.PutRefreshesExpiry` — 重新 put 重置过期
6. `TtlCacheLeaveCallbackTest.LazyExpiryNotifiesExpiredWithKeyValue` — get 触发，reason=Expired 且 key/value 正确
7. `TtlCacheLeaveCallbackTest.ExistsAlsoPurgesAndNotifiesExpired`
8. `TtlCacheTest.PurgeExpiredRemovesOnlyExpiredAndReturnsCount`
9. `TtlCacheTest.SizePurgesExpiredFirst` — size 排除过期且回调已发
10. `TtlCacheLeaveCallbackTest.RemoveForwardsExplicit` — 未过期 key
11. `TtlCacheLeaveCallbackTest.InnerEvictionForwardedAsEvicted` — 容量 1 的 inner，两次未过期 put
12. `TtlCacheLeaveCallbackTest.ZombieEvictionConvertedToExpired` — 过期后未触碰，随后 put 挤出，对外报 Expired
13. `TtlCacheLeaveCallbackTest.NoCallbackForCapacityZeroInner` — 容量 0：静默、无回调、expiryMap_ 不留僵尸
14. `TtlCacheTest.ConcurrentSmokeWithSteadyClock` — 真实时钟 5 线程读写 + 50ms TTL（仿 LruCache_test.cc 并发测试形状）
15. `TtlCacheLeaveCallbackTest.RemoveExpiredKeyReportsExplicit` — 固化分类规则 2 的决策

## 实施步骤

1. 写 `src/Ttl/TtlCache.h`（含骨架注释：锁序、"不得共享裸 inner"、分类规则）
2. 写 `tests/TtlCache_test.cc`（15 个用例 + ManualClock + LeaveRecorder）
3. 重新 configure + 构建 + 跑测试

## 验证

```bash
cmake -S . -B build -DBUILD_TEST=ON   # 必须重新 configure，file(GLOB) 才能发现新测试文件
cmake --build build
ctest --test-dir build --output-on-failure
```

预期：原有 197 用例 + 新增 15 用例全绿。

## 关键风险（已由设计消解）

- **锁内通知**（最大坑）：所有 `notifyLeave` 都在 `mutex_` 释放后，保住"回调可重入"的阶段 0 保证
- Sharded inner 多条目离开：缓冲是 vector，单次 inner 调用可产生 N 条事件
- 容量 0 inner：put 后用 `inner->exists(key)` 校验兜底，expiryMap_ 不留永久僵尸
