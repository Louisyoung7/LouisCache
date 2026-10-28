---
title: "SemanticCache 依赖装配采用构造函数注入"
date: 2026-10-25
status: Accepted
---

# ADR-0001: SemanticCache 依赖装配采用构造函数注入

## 背景

SemanticCache 需要接入三类运行时对象：`Embedder`（向量化）、`VectorIndex`（向量检索）与 `EntryStore`（条目存储，即 `Policy<uint64_t, CacheEntry>` 的具体实现）。三者都是接口，但生命周期与所有权语义并不相同，装配方式直接决定了回调契约与不变量能否被守住。

- CON-001: `EntryStore` 的 `LeaveCallback` 只能有一个写者，且 `onLeave` 是索引与精确映射的唯一清理出口；若两个缓存共用一个 store，后注册者会覆盖回调，`vectorIndex_->size()` 与 `store_->size()` 将脱节（见 design §4.1、§4.3）。
- CON-002: `VectorIndex` 的 id 空间按缓存实例独立（`nextId_` 从 0 开始）。跨缓存共享索引会让 id 撞车——A 的向量可能命中 B 的条目，产生"用 A 的问题召回 B 的答案"的幽灵命中。
- CON-003: `Embedder::embed()` 是 const 纯函数、无状态，模型实例通常希望被多个缓存或 namespace 共享，以摊薄加载成本。
- CON-004: 装配顺序存在硬约束：`embedder->dim()` 必须等于 `vectorIndex->dim()`，否则每次 `add` 都抛异常，因此 embedder 必须先于 index 构造。
- CON-005: 项目是 header-only 库，`Options` 目前是纯值配置，可映射配置文件、可被测试参数化；核心头文件不宜引入具体策略实现。
- CON-006: 阶段 1 之后要支持多种淘汰策略（LRU / LFU / ARC / LRU-K）与 TTL 装饰器，核心类不应感知具体策略。
- CON-007: `onLeave` 需要凭条目内的 prompt 反查并擦除 `exactIndex_` 映射，因此条目值类型必须对注入方可见。

## 决策

- DEC-001: 三个运行时依赖全部由构造函数注入。主构造签名为 `SemanticCache(Options, std::shared_ptr<const Embedder>, std::unique_ptr<VectorIndex>, std::unique_ptr<Policy<uint64_t, CacheEntry>>)`。
- DEC-002: 用持有类型编码所有权契约：`Embedder` 用 `shared_ptr<const Embedder>`（允许共享）；`VectorIndex` 用 `unique_ptr`（必须独占）；`EntryStore` 用 `unique_ptr`（必须独占且由本缓存安装回调）。
- DEC-003: `Options` 只保留可序列化的值配置（capacity / hitThreshold / searchK / exactIndexEnabled）；注入路径下 `capacity` 不生效，容量由注入的 store 决定。
- DEC-004: 条目值类型提升为顶层 `CacheEntry { prompt, response }`，使外部能构造任意 `Policy<uint64_t, CacheEntry>` 实现。
- DEC-005: 保留三参便捷构造，按 `options.capacity` 创建默认 `LruCache` 后转调主构造；策略切换由调用方选择具体类，TTL 通过注入 `TtlCache` 装饰器组合，二者都不进入核心类。
- DEC-006: 若将来需要配置驱动的对象装配，放到 `server/` 层的 builder 实现（阶段 3），核心库不承担服务定位职责。

## 备选方案

### ALT-001: 三类对象的工厂函数全部注册进 `Options`

- 否决原因：`Options` 会从纯配置退化为服务定位器，与 CON-005 冲突；共享 `Embedder` 只能靠工厂隐式捕获 `shared_ptr`，违背 CON-003 对"共享应显式"的要求；装配顺序被藏进缓存内部，CON-004 的 `dim` 校验失去调用方控制；header-only 场景下 `Options.h` 还需拖入具体策略实现，放大编译面。

### ALT-002: `Options` 增加策略枚举，由 SemanticCache 内部 switch 出具体类

- 否决原因：满足 CON-006 的"可选策略"，但 `SemanticCache.h` 必须 include `LruCache.h` / `LfuCache.h` / `ArcCache.h` / `LruKCache.h` 全部实现，用户无法只为自己使用的策略付出编译成本；枚举也无法表达 `TtlCache` 这类装饰器组合。该职责更适合 DEC-006 所述的 server 层 builder。

### ALT-003: 三类依赖统一用 `shared_ptr` 注入

- 否决原因：类型系统不再阻止跨缓存共享 `EntryStore` 与 `VectorIndex`，CON-001 的回调覆盖和 CON-002 的 id 撞车只能靠文档约束；`unique_ptr` 能在编译期拦下这类误用。

### ALT-004: 值类型继续用 `std::string`，另建 id → prompt 映射供回调使用

- 否决原因：`onLeave` 在锁外被调用，需要额外维护一份与 store 同生命周期的反向映射并保证其一致性，等于把 `CacheEntry` 的单一事实来源拆成两份；直接让值类型携带 prompt 成本更低，也满足 CON-007。
