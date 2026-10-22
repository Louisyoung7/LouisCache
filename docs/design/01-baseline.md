# 当前基线（阶段 0 已完成）

> **分卷**：第 2 卷「当前基线」　|　**文档版本**：v0.1　|　**最近更新**：2026-10-14　|　**对应原文档**：第二节（§2）　|　**关联**：[主文档 · 项目定位与索引](../design.md) · [总体架构](02-architecture.md)

---

阶段 0 的目标是给语义层铺好地基，两条主线：**条目离开的统一出口**、**测试基建**。

| 能力 | 状态 | 落点 |
|---|---|---|
| `Policy` 抽象接口：`put` / `get`×2 / `remove` / `size` | ✅ | [Policy.h](../../src/Policy.h#L7-L42) |
| `LeaveReason { Evicted, Explicit, Expired }` + 回调注册 | ✅ | [Policy.h](../../src/Policy.h#L5-L38) |
| `remove()` 提升为虚接口（原先仅是 `LruCache` 上的非虚函数） | ✅ | [Policy.h](../../src/Policy.h#L26) |
| 6 种策略全部接入回调：LRU / LRU-K / Sharded LRU / LFU / Sharded LFU / ARC | ✅ | `src/LRU`、`src/LFU`、`src/ARC` |
| **锁外分发**回调（回调内可安全重入缓存） | ✅ | 各实现均"锁内收集、锁外 `notifyLeave`" |
| **内部流转静默**（复合结构内部迁移不通知） | ✅ | [LruKCache.h](../../src/LRU/LruKCache.h)、[ArcCache.h](../../src/ARC/ArcCache.h) |
| 单元测试基建（Google Test + Conan + CTest） | ✅ | [tests/](../../tests)，6 文件 **175 用例**全绿 |

## 2.1 地基已具备、语义层可直接依赖的三条语义

这是阶段 0 真正的产出，阶段 1 的编排逻辑必须建立在其上：

1. **所有离开路径统一走 `LeaveCallback(key, value, reason)`**：容量淘汰 → `Evicted`、`remove()` → `Explicit`、过期 → `Expired`。语义层只需注册一次回调，即可在同一处同步清理外部索引。
2. **回调在缓存内部锁全部释放后触发**：回调内可安全重入缓存的任意方法，且能观察到条目离开后的最终状态。
3. **复合结构内部流转对外不可见**：`LruKCache` 的历史队列晋升、`ArcCache` 的 T1→T2 迁移与幽灵缓存挤出/命中均静默，只有条目真正离开缓存才通知。

## 2.2 阶段 0 遗留、需在阶段 1 顺手补齐的缺口

| 缺口 | 影响 | 处理 |
|---|---|---|
| `Expired` 枚举已定义但**无生产者** | TTL 能力缺失 | 阶段 1 实现 `TtlCache` 装饰器 |
| `Policy` 无 `contains()` / `capacity()` / `clear()` | `contains` 仅在 `LruCache` 上；语义层要判断驻留需绕路 | 按需补入 `Policy`，避免为将来预留 |
| 无 `Stats` / `Options` | 阈值调优无数据反馈 | 阶段 1 随 `SemanticCache` 一起设计 |
| 回调 value 为大对象时按 `const&` 传递，但 `put` 为传值语义 | 语义层 `Entry` 含 prompt+response，拷贝成本需留意 | 阶段 1 实测后再决定是否加移动重载 |

---

**文档导航**　[‹ 主文档 · 项目定位与索引](../design.md)　·　[文档索引](../design.md#文档索引)　·　[下一卷：总体架构 ›](02-architecture.md)
