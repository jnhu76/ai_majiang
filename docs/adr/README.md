# Architecture Decision Records

本目录是 `ai_majiang` 的正式架构决策记录（ADR）。每条决策一个文件，编号从
`0001` 递增；文件名格式 `<编号>-<短主题>.md`。决策一旦 Accept 后要变更，
不修改原文，而是新增一条新 ADR 并在旧 ADR 顶部标注被取代关系。

## 状态

- `Proposed` — 讨论中，尚未成为约束。
- `Accepted` — 已被 Owner 采纳，对后续 PR 有约束力。
- `Superseded by <n>` — 被后续 ADR 取代。

## 已落盘的 ADR

| 编号 | 标题 | 状态 | 日期 |
|---|---|---|---|
| [0001](0001-npc-offline-authoring-pipeline.md) | NPC Offline Authoring and Runtime Behavior Separation | Accepted | 2026-11-11 |

## 评审基线中尚未落盘的决策（待整理）

2026-11-11 的独立架构评审报告（审计基线 `29e6c5c`，外部交付物，未入库）
提出了 ADR-01～06 共六项决策。它们**尚未整理为独立的 ADR 文件**；本目录
不创建占位空文件。待逐项整理时按 `0002+` 顺序编号，并在下表登记映射。

| 评审编号 | 主题 | 落盘状态 |
|---|---|---|
| ADR-01 | 确定性麻将核心（GameSession 独占权威状态） | 待整理 |
| ADR-02 | Axmol 2.11.5 LTS 三端宿主 | 待整理 |
| ADR-03 | MNN / llama.cpp 可替换推理后端，默认无模型 | 待整理 |
| ADR-04 | 确定性 SocialDirector 与受约束对话 | 待整理；由 [ADR-0001](0001-npc-offline-authoring-pipeline.md) 扩展 |
| ADR-05 | 分层立绘与局部序列帧动画 | 待整理 |
| ADR-06 | 声明式角色包与离线资源 | 待整理；由 [ADR-0001](0001-npc-offline-authoring-pipeline.md) 扩展 |

在正式 ADR 落盘之前，这些决策以评审报告与 `docs/ARCHITECTURE.md` 的现行
约定为准；两者冲突时以仓库内文档与 `AGENTS.md` 为准。

## 写作约定

- 每条 ADR 包含：Status / Context / Decision / Alternatives / Rationale /
  Consequences / Invariants / Security Boundaries / Integration Points /
  Migration / Acceptance Criteria / Revisit Triggers。
- 决策必须说明它扩展或取代了哪些既有决策，不得建立并行的第二套系统。
- `Accepted` 只批准架构原则；实现与设备验证状态在正文 Acceptance Criteria
  中如实标注，不得默认已实现。
