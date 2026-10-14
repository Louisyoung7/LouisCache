# LouisCache 语义缓存扩展：架构与实施规划

> 本文档回答三个问题：**做什么**（目标与边界）、**怎么做**（架构与关键决策）、**下一步做什么**（分阶段任务清单与验收标准）。
> 进度基线：**阶段 0（地基）已完成**，详见 [当前基线](design/01-baseline.md)。

*文档版本：v0.1　|　最近更新：2026-10-14　|　文档角色：总览 + 索引根（各子卷内容范围见 [文档索引](#文档索引)）*

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

## 文档索引

| 文件 | 内容范围 | 适用场景 | 关联关系 |
|---|---|---|---|
| [design/01-baseline.md](design/01-baseline.md) | 阶段 0 基线：能力清单、三条可依赖语义、遗留缺口 | 了解地基现状、评估语义层可依赖的前提 | 地基，无上游；被 [02-architecture](design/02-architecture.md)、[03-design-decisions](design/03-design-decisions.md)、[04-roadmap](design/04-roadmap.md) 引用 |
| [design/02-architecture.md](design/02-architecture.md) | 总体架构图、get / put 数据流、模块自研分界、分层职责 | 理解模块划分与端到端流程 | 依赖 [01-baseline](design/01-baseline.md)；流程取舍由 [03-design-decisions](design/03-design-decisions.md) 展开 |
| [design/03-design-decisions.md](design/03-design-decisions.md) | 7 条关键设计决策（一致性枢纽、键选型、写入顺序、锁序、阈值、度量口径、零依赖） | 评审 / 实现前确认关键取舍；§4.1、§4.3 被架构与路线图引用 | 承接 [02-architecture](design/02-architecture.md)；约束 [04-roadmap](design/04-roadmap.md) 落地 |
| [design/04-roadmap.md](design/04-roadmap.md) | 阶段 0–3 的目标、文件级交付物、接口草案、验收标准 | 排期与验收；逐阶段落地 | 引用 [01-baseline](design/01-baseline.md)；受 [03-design-decisions](design/03-design-decisions.md) 约束 |
| [design/05-principles.md](design/05-principles.md) | 5 条贯穿性原则 | 任何阶段改动前的准绳 | 贯穿全部子卷 |

## 章节映射与交叉引用

原文档章节与拆分后子卷的一一对应关系，便于按旧编号定位、按新文件维护：

| 原文档章节 | 现行位置 | 主要锚点 |
|---|---|---|
| §1 项目定位与边界 | 本文档 [一、项目定位与边界](#一项目定位与边界) | `#11-现状`、`#12-扩展目标`、`#13-使用者与边界`、`#14-明确不做` |
| §2 当前基线（阶段 0） | [design/01-baseline.md](design/01-baseline.md) | `#当前基线阶段-0-已完成`、`#21-地基已具备语义层可直接依赖的三条语义`、`#22-阶段-0-遗留需在阶段-1-顺手补齐的缺口` |
| §3 总体架构 | [design/02-architecture.md](design/02-architecture.md) | `#总体架构`、`#31-get-数据流`、`#32-put-数据流`、`#33-各模块的自研--现成分界`、`#34-分层职责` |
| §4 关键设计决策 | [design/03-design-decisions.md](design/03-design-decisions.md) | `#41-一致性枢纽leavecallback-是唯一清理出口`、`#42-entrystore-用-id-而非-prompt-作-key`、`#43-写入顺序先存储后索引`、`#44-锁序持有语义层锁时不得调用-entrystore`、`#45-阈值判定--策略层`、`#46-distance--similarity-转换挡在适配层`、`#47-零依赖原则与可选开关` |
| §5 分阶段任务清单 | [design/04-roadmap.md](design/04-roadmap.md) | `#阶段-0--地基--已完成`、`#阶段-1--mvp保持零依赖`、`#阶段-2--真实向量化与生产化`、`#阶段-3--评测与差异化` |
| §6 贯穿性原则 | [design/05-principles.md](design/05-principles.md) | `#贯穿性原则` |

拆分过程中已同步修正的交叉引用（保持可追溯，未改动设计内容）：

- 原 §3.2 中「理由见 §4.3」→ [design/03-design-decisions.md §4.3](design/03-design-decisions.md#43-写入顺序先存储后索引)
- 原 §5 阶段 0 中「见第二节」→ [design/01-baseline.md](design/01-baseline.md)
- 外部文档 [ttl-cache-decorator-plan.md](ttl-cache-decorator-plan.md) 中的「design.md §4.1」→ [design/03-design-decisions.md §4.1](design/03-design-decisions.md#41-一致性枢纽leavecallback-是唯一清理出口)

## 文档维护约定

为避免整体文档再度膨胀，后续变更遵循：

1. **单一主题单一文件**：每个子卷仅承载一个章节主题；新增长内容应新增子卷，而非堆回主文档。
2. **主文档不承载细节**：主文档只保留「项目定位」与索引；架构、决策、路线图等细节改动一律落到对应子卷。
3. **引用统一用相对路径 + 锚点**：跨子卷与外部引用统一写作 `design/<file>.md#<anchor>`，保证链接可追溯。
4. **版本标识一致**：各子卷版本号与主文档同步；任一子卷实质变更时，同步更新其顶部「最近更新」。

---

**关联文档**：[TtlCache 装饰器实现计划](ttl-cache-decorator-plan.md) · [为什么 ShardedLruCache 禁止拷贝和移动](为什么ShardedLruCache禁止拷贝和移动.md)
