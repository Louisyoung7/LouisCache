# 架构决策记录（ADR）索引

记录仓库级影响面超过单个文件的设计决策。模块内部的决策放在各模块自己的
`docs/adr/`（如 [modules/semantic/docs/adr](../../modules/semantic/docs/adr)），
两边各自从 0001 编号。

新文件命名：`adr-NNNN-<小写 kebab-case 标题>.md`，编号不足 4 位补零；
被新决策取代时旧文件 front matter 改为 `status: Superseded` 并注明
`superseded_by`，同时在下表追加新行。

| 编号 | 标题 | 状态 | 日期 | 摘要 |
|---|---|---|---|---|
| [ADR-0001](adr-0001-benchmark-thread-pool.md) | 多线程压测自建线程池而非用 Google Benchmark Threads(n) | Accepted | 2026-11-07 | 锁争用压测需要每轮跨线程栅栏与诊断计数聚合，且线程创建实测造成 5 倍膨胀，故自建常驻线程池 + 双条件变量，弃用框架自带多线程 API。 |
