# 架构决策记录（ADR）索引 — semantic 模块

记录 SemanticCache 模块内部影响面超过单个文件的设计决策。仓库级决策放根目录
[docs/adr](../../../../docs/adr)，两边各自从 0001 编号。

新文件命名：`adr-NNNN-<小写 kebab-case 标题>.md`，编号不足 4 位补零；
被新决策取代时旧文件 front matter 改为 `status: Superseded` 并注明
`superseded_by`，同时在下表追加新行。

| 编号 | 标题 | 状态 | 日期 | 摘要 |
|---|---|---|---|---|
| [ADR-0001](adr-0001-semantic-cache-dependency-injection.md) | SemanticCache 依赖装配采用构造函数注入 | Accepted | 2026-10-25 | Embedder / VectorIndex / EntryStore 三类依赖由构造函数注入，用持有类型编码所有权契约（shared_ptr 共享、unique_ptr 独占），Options 只留纯值配置；拒绝工厂注册与策略枚举，避免 Options 退化为服务定位器。 |
| [ADR-0002](adr-0002-duplicate-put-upsert.md) | SemanticCache 重复 put 采用 exactIndex_ 覆盖写（upsert） | Accepted | 2026-10-29 | 重复判定只以 exactIndex_ 为唯一依据：命中即先 remove 旧条目再走统一写入序列，清理交由 onLeave 单一出口，保证同 prompt 只留最新一条且不占两份容量；拒绝借 search 判重与按 id 降序返回最新两种方案。 |
