# NPC Teacher–Student 架构 ADR 落地 — 交付报告

- **任务**：把"场外大模型辅助创作与评审、场内小模型辅助执行、确定性系统兜底"提交至以 `dev` 为目标的 ADR PR。
- **日期**：2026-11-11
- **落盘基线**：`dev@5db26bd`（评审报告基线 `29e6c5c` 为其祖先，无冲突）
- **交付物**：分支 `docs/npc-teacher-student-adr`，契约修复 commit `4e2360898a4661fb916a121cfae921ece0cef0a3`，PR [jnhu76/ai_majiang#12](https://github.com/jnhu76/ai_majiang/pull/12)（目标 `dev`，已于 2026-11-11 合并，merge commit [`7ac3cc5`](https://github.com/jnhu76/ai_majiang/commit/7ac3cc5d90a1e06f4f0a3275f13b2e9625d7ee12)）
- **本报告**：交付记录，独立成 PR；**允许未来不合并**。文档链接固定到上述 #12 提交。若 #12 的契约继续改动，更新报告所引用的提交和事实。

---

## A. Repository Reality

- 重新 fetch 后 `origin/dev` HEAD = `5db26bd`（Merge PR #11，含 P2 白板音频修复）；评审基线 `29e6c5c` 是其祖先，无需 rebase，未覆盖他人工作。
- 交付基线中没有 `docs/adr/` 目录；评审报告中的 ADR-01～06 均未落盘，评审报告本身为外部交付物（未入库）。本次在 ADR 索引中如实标注"待整理"，**未创建占位空文件**。
- 实际接口核实：`voice::Director`/`Bank` 的 `(event_type, fact_key, speaker)` 精确匹配、rotation/weighted/llm 策略与 recent 窗口，和评审描述一致；未发现新增决策与现有 `GameSession`、评审 ADR-04/ADR-06 边界的冲突。
- 当前生成入口为 `ILlmBackend::generate(prompt, speaker)`。GameSession
  当前配套 DemoRuleSet 和 `DemoTileTapped`。本报告 D 节的 NPC 接口为 Proposed Interfaces。

## B. Architecture Decision（三方职责）

| 路径 | 负责方 | 职责 |
|---|---|---|
| 场外创作（开发期） | Teacher Model（GPT-6 级）+ 人工编剧 | 发现场景、推演人格反应、创作候选台词、提出情绪/关系规则**提案**；一切产出必须经 Schema/语义校验 + 覆盖与重复分析 + 人工审核 |
| 场内执行（玩家设备） | 确定性运行时 | 场景匹配、情绪演化、节奏/沉默/防循环、加权候选选择；端侧小模型仅可选地对**已提供候选 LineId** 受约束排序 |
| 规则与事实 | `GameSession` 公开投影 | 唯一事实来源；任何模型不可触牌墙、手牌、合法动作、计分、回合顺序或权威状态 |
| 兜底与发布 | 人工审核 + 确定性策略 | 未审核内容不进产品；候选排序失败且作用域仍有效时回退至同一候选集的确定性选择；过期直接丢弃 |

**大模型是开发工具依赖，不是游戏运行时依赖。**

## C. Data Contract

三层内容模型：

1. **Scenario Catalog**（app 持有，随版本发布）：typed trigger（首个切片封闭枚举 4 种）+ 声明式 guard（纯谓词 AND 组合）+ `priority/cooldown/silence_policy/max_followups/observer_filter`。
2. **BehaviorPack v1**（每角色一份）：按 `scenario_id` 声明反应（`trait_bands` 人格带、`observer_scope`、`emotion_rule`、可选 `relationship_rule`、`dialogue_act`、`gesture_hint`、台词引用）+ 内嵌台词库（text/voice/slots/weight）。反应按数组取首个命中，
   `dialogue_act` 必须与引用台词相同。
3. **CharacterPack**（评审 ADR-06）：身份、SOUL、traits、声线、形象；以 `(pack_id, version, content_hash)` **精确引用恰好一份** BehaviorPack，整局固定快照。

匹配与情绪计算读取同一冻结快照，状态拥有者统一提交。显式沉默和冷却都保留
反应的状态变化。`neutral/0` 保持原情绪。首个切片在换手时重置情绪，新包随下局
激活并重建该座状态。台词可覆盖反应的手势。

音频契约区分静态 WAV、绑定全部公开槽值的有限 WAV 变体、动态本地 TTS 和
同一句字幕回退。投递校验无法验证槽位时静默，禁止用其他槽值的录音补位。
契约：[BEHAVIOR-PACK-V1.md](https://github.com/jnhu76/ai_majiang/blob/4e2360898a4661fb916a121cfae921ece0cef0a3/docs/npc/BEHAVIOR-PACK-V1.md)。

## D. Module Ownership

下表的游戏内 NPC 模块为 Proposed Interfaces，现有实验代码见 A 节。

| 模块 | 负责 | 不负责 |
|---|---|---|
| `SocialDirector` | 发言节奏权威：优先级、双冷却、一次回应链、epoch 失效 | 不匹配场景、不算情绪、不排序台词 |
| `CharacterRuntimeState` | 情绪/关系/记忆/`recent_delivered`（拥有者提交 reducer 结果，交付回执更新近期记录） | — |
| `ScenarioMatcher` / `EmotionReducer`（新增纯函数） | 目录/行为包数据驱动的匹配与确定性状态更新 | 无 I/O、无模型、无时钟 |
| `IDialoguePolicy` | 在本次提供候选集内选择（含 Silence） | 不产生新事实、不接受未提供 ID |
| `ITextGenerationBackend` / `ModelManager` | 拟议 `OutputShape.allowed_ids` 约束下的候选排序 | 不理解麻将 |
| `ICharacterAnimator` | `PresentationIntent`（情绪态 + 手势枚举） | 不接触提示词 |
| `IAssetRepository` / `ContentPackManager` | 安装、校验、pin 版本 | 不创作内容 |
| Teacher Model | 开发期提案 | 不参与任何运行时权威计算 |

`voice::Director` 与 P2 银行提供迁移来源。模型故障由推理运行时报告，
app 可生成界面用的 `SystemCue`。它与进入场景匹配的公开牌局事件、等待和
回应 `SocialCue` 分开。

## E. Example Scenarios

[示例 JSON](https://github.com/jnhu76/ai_majiang/tree/4e2360898a4661fb916a121cfae921ece0cef0a3/docs/npc/examples)
配合下文 Golden 预期描述三个案例。

1. **`player.idle.long`**。姜小快催牌并变为 annoyed，温阿默微笑沉默。
   原始台词实际交付后，温阿默在关系和冷却允许时打趣一次，回应是终止句。
   催牌被冷却时，原始反应仍提交情绪，交付和回应均为零。
2. **`game_action.meld.claimed_of_my_discard`**。actor 是碰牌者，target 是被碰的舍牌者。
   姜小快作为 target 时优先匹配包含当前事件在内的重复碰牌守卫，字幕引用
   `{claim_count}`；普通碰牌走抱怨。温阿默作为旁观者可提议称赞，作为
   target 则静默并保持已有情绪。最终发言按 SocialDirector 调度。
3. **`continuity.hand_opening_after_player_win`**。首次摸牌时读取已确认的上一手
   摘要。姜小快在玩家连胜至少两手时用 `{streak}` 台词，否则走普通放话。
   上一手胜负未确认时场景不匹配。

## F. Evaluation

- **覆盖**：名义（静态）/可触发（受控时钟夹具）/实际交付（真实对局日志）三级口径；禁止人格×情绪×事件笛卡尔积分母；真实轨迹高频未匹配事件回流给 Scene Explorer。
- **质量**：人格辨识三选一盲测（对照 33% 机会水平 + 混淆矩阵）、事实正确率（已发布关键事实错配为发布阻断项）、语义重复率、人工趣味成对盲评；LLM 自评仅用于排序人工审核队列，不作质量证据。
- **回归**：规则行为、旧场景匹配、声线映射、动画合法性、离线能力、性能预算、过期响应清理 7 条门。
- **Golden Scenario Corpus**：固定有序事件、cue、tick、包版本和初始状态。
  三个用例规定预期匹配、情绪、台词及交付结果。Runtime NOT IMPLEMENTED。
- 契约：[EVALUATION-PLAN.md](https://github.com/jnhu76/ai_majiang/blob/4e2360898a4661fb916a121cfae921ece0cef0a3/docs/npc/EVALUATION-PLAN.md)。

## G. Backward Compatibility

- P2 TSV 银行经离线转换器导入 BehaviorPack（计划任务；经核实的公开事件、角色 traits、`tts_sid→voice_id` 映射和原权重）。
- P2 CLI、夹具与已归档报告（`docs/experiments/2026-11-*`）原样保留、可复现；fixed/weighted 继续作为后续模型臂的对照基线。
- 迁移完成后 `voice::Director` 的轮转/近期逻辑并入新的确定性 `IDialoguePolicy`，旧接口仅留给实验探针，不长期双轨。

## H. ADR Changes

新增：

- `docs/adr/README.md` — ADR 索引；评审 ADR-01～06 标注"待整理"
- `docs/adr/0001-npc-offline-authoring-pipeline.md` — **NPC Offline Authoring and Runtime Behavior Separation**（Accepted；含 Status/Context/Decision/Alternatives/Rationale/Consequences/Invariants/Security Boundaries/Integration Points/Migration/Acceptance Criteria/Revisit Triggers）
- `docs/npc/`：`README.md`、`SCENARIO-MODEL.md`、`BEHAVIOR-PACK-V1.md`、`AUTHORING-PIPELINE.md`、`EVALUATION-PLAN.md`、`examples/`（4 个 JSON）、`npc-v1.schema.json`、`validate_examples.py`

更新：`docs/ARCHITECTURE.md` → v0.2（非协商属性补充教师模型边界 + 分离章节 + 交付位置，未削弱 v0.1 属性）；`README.md` 加 ADR 入口链接。#12 含文档、数据和开发期校验脚本。

## I. Migration Roadmap

| 类别 | 内容 |
|---|---|
| 必须立即开发 | Headless 最小验证切片：目录 v1 加载器 + ScenarioMatcher + EmotionReducer + 最小 SocialDirector 节奏控制 + 确定性 `IDialoguePolicy` + Golden 运行器，覆盖三个示范场景 |
| 应当预留接口 | `IDialoguePolicy` 模型排序臂（拟议 `OutputShape.allowed_ids`）；行为包安装校验接入 `ContentPackManager`；P2 银行导入转换器 |
| 暂时不做 | 受约束生成、跨包依赖管理、Mood 独立存储、内容目录服务、Agent 框架/ECS/脚本 |
| 需真实实验才决定 | Pocket-0.3B、尚无已完成对照结果的 Qwen3-0.6B 与 weighted 的盲测、DialogueAct 两段式选择、蒸馏/微调（含 Dataset Contract 与数据许可） |

## J. GitHub Delivery

| 项 | 值 |
|---|---|
| 基线 | `dev@5db26bd` |
| 交付分支 / commit | `docs/npc-teacher-student-adr` / [`4e236089`](https://github.com/jnhu76/ai_majiang/commit/4e2360898a4661fb916a121cfae921ece0cef0a3) |
| PR | [#12](https://github.com/jnhu76/ai_majiang/pull/12) → `dev` |
| Headless CI | [修复提交的三平台 CI](https://github.com/jnhu76/ai_majiang/actions/runs/43055501176) |
| 离线检查 | **PASS**，4 份 JSON Schema 和引用校验，2 种额外合法音频形状、1 个合法提案修订引用和 30 项反例 |
| Runtime / 资产 | **NOT IMPLEMENTED**，Golden 预期为契约，未验证 C++ NPC 运行时及实际录音 |

---

## 最终架构裁决（8 问）

1. **场外 Teacher Model 是否一等概念？** 是——作为**开发期内容流水线的一等契约**（提案进、声明式包出、全程可审计可版本化），但明确为"开发工具依赖、非运行时依赖"，玩家设备上不存在其调用路径。
2. **BehaviorPack 独立还是子资源？** 独立版本化工件；CharacterPack 按内容哈希精确引用恰好一份——"有引用、无依赖图"，兼顾跨包覆盖静态分析与运行时零解析成本。
3. **情绪是确定性规则还是小模型决定？** 确定性规则计算（EmotionReducer 纯函数 + 声明式规则数据）；教师模型只能提议规则、人工审核后编译为数据；端侧模型永不设置情绪。
4. **小模型第一阶段选 LineId、DialogueAct 还是其他？** 直接选 **LineId**（已过滤候选集内受约束选择/排序，`allowed_ids` 硬校验、失败走确定性兜底）；DialogueAct 留作候选集过大时的两段式降级。
5. **场景增长如何不使 SocialDirector 巨型化？** 它只拥有不随场景数变化的节奏语义；增长落在数据（目录条目 + 行为包反应），扩展只经三条受控通道（新 trigger/guard kind、新 DialogueAct/手势枚举值），禁止场景特定分支。
6. **如何衡量场外内容确实提高趣味性？** 唯一发布判据是预注册盲测成对对照（新内容臂 vs fixed/weighted 基线：成对偏好显著、人格辨识超机会水平、事实错配为零、重复率不劣化）；覆盖率与 LLM 评审只是必要条件。
7. **永不蒸馏/微调，架构仍有价值吗？** 有。教师模型可以补充已观察到的内容缺口（P2 的 17/43 单候选、无人格差异、无记忆互动），由确定性路径使用获批内容；小模型只是可选增强层。
8. **下一个最小验证切片？** 目录加载、Matcher、EmotionReducer、最小节奏控制、
   确定性策略与 Headless Golden 运行器。验收目标是用固定输入复现三个场景。

---

**最终目标对齐**：不是给 NPC 复杂的 Agent 内部结构，而是让玩家感到桌上的三个角色有不同的性格、记得刚才发生的事、每局有新的互动——教师模型负责让这份"性格与记忆"在内容上足够厚，确定性运行时负责让它在每台设备上可靠兑现。
