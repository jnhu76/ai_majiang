# 场景模型（Scenario Model）— v1 契约草案

归属：[ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。
本文定义场内 NPC 的场景/情绪执行模型。目标是**够表达未来互动、但不过度
复杂**：结构放进共享的场景目录，差异放进角色行为包，状态更新全部确定
性。

## 1. 管线与组件职责

```text
PublicGameEvent（麻将公开事件投影）
SocialCue（表现/社交层触发，如玩家等待超时）
      |
      v
ScenarioMatcher ──场景目录（声明式数据，app 持有）
      |
      v
BehaviorContext（冻结值快照：公开事实 + 各观察者心理状态投影）
      |
      v
EmotionReducer ──行为包情绪规则──> CharacterRuntimeState（情绪/关系/记忆）
      |
      v
SocialDirector（节奏权威：优先级、冷却、沉默、一次回应链）
      |
      v
IDialoguePolicy（候选选择：加权轮转默认 / 可选端侧模型排序）
      |
      v
ValidatedLine + PresentationIntent（字幕/TTS/动画）
```

| 组件 | 只负责 | 不负责 |
|---|---|---|
| ScenarioMatcher | 用目录数据把事件/cue 匹配为 `scenario_id` + BehaviorContext | 不改情绪、不选台词 |
| EmotionReducer | 纯函数更新情绪/关系/记忆（按行为包声明式规则） | 不决定谁发言 |
| SocialDirector | 发言节奏、冷却、沉默、`exchangeId` 回应链、epoch 失效 | 不匹配场景、不算情绪、不排序台词 |
| IDialoguePolicy | 在**本次提供的**候选集内选择/排序（含 Silence） | 不产生新事实、不接受未提供 ID |
| 校验器（ValidatedLine 构造） | ID 存在性、人格/声线/槽位/作用域复核 | — |

设计规则：**每个字段只属于一层，不为每个字段建独立引擎**。目录描述“什
么事发生了”；行为包描述“这类角色对此有何反应”；运行时状态描述“这个座
位现在怎么样”。

## 2. 场景目录（Scenario Catalog）

由 app 持有并随应用版本化（`catalog_revision`）。它是行为包的**词汇表契
约**：行为包只允许引用目录中存在的 `scenario_id`。

### 2.1 字段：必填 vs 派生

| 字段 | 归属 | 说明 |
|---|---|---|
| `scenario_id` / `title` / `category` | 目录必填 | 稳定 ID、人类可读名、分类（见 §3） |
| `trigger` | 目录必填 | typed trigger：`kind` + 封闭参数（见 §2.2） |
| `guards` | 目录可选 | 声明式守卫列表，AND 语义（见 §2.3） |
| `priority` | 目录必填 | `low / normal / high / critical`；重要牌局 cue 可抢占闲聊 |
| `cooldown` | 目录必填 | `per_scenario_s` + `per_speaker_s` 默认值 |
| `silence_policy` | 目录必填 | `allow / prefer / must_speak`（见 §6） |
| `max_followups` | 目录必填 | 本场景一次匹配允许的一次性回应数，v1 ∈ {0, 1} |
| `observer_filter` | 目录必填 | 候选观察者集合，v1 封闭枚举：`all_npc / all_npc_except_actor / all_npc_except_current_player / actor_only` |
| Actor / Observer | **运行时派生** | 来自事件载荷与 `observer_filter`，不写入目录 |
| Personality / Emotion / Relationship / Memory | **运行时派生** | 匹配时从 `CharacterRuntimeState` 取冻结投影 |
| DialogueAct / 台词 / 演出 | **行为包** | 反应层声明，目录不涉及 |

### 2.2 Trigger kinds（v1 封闭枚举）

| kind | 参数 | 来源 |
|---|---|---|
| `domain_event` | `event_type`（公开事件类型）+ 可选 `fact_filter` | 麻将公开事件投影 |
| `player_idle` | `duration_band` ∈ `short / medium / long` | SocialCue（表现层，§5） |
| `hand_phase` | `phase` ∈ `dealt / first_draw / settling / finished` | 公开事件 |
| `hand_continuity` | `last_hand_outcome` + 可选 `streak_min` | 已确认的上一手公开摘要（记忆） |
| `exchange_followup` | `requires_act`（上一条已交付台词的 DialogueAct） | SocialDirector 发起的回应链 |

新增 kind = 代码 + Schema 版本共同演进（有意摩擦），组合只允许目录内多
守卫 AND，不允许用户内容写表达式或执行代码。

### 2.3 Guard kinds（v1 封闭枚举，全部纯谓词）

`game_active`、`speaker_not_current_player`、`speaker_not_actor`、
`speaker_enabled`、`emotion_in{set}`、`fact_query`（封闭查询词汇：
`hand_claim_count_by_seat_min`、`last_hand_winner`、`win_streak_min`、
`relationship_affinity_max` 等）、`recent_line_same{window}`、
`hand_number_min`、`locale_available`。

`fact_query` 只查询公开事实与已确认摘要（如“本手同一座位已从我这碰牌 ≥2
次”），永不查询暗牌、牌墙或未确认胜负。

## 3. 场景分类体系（初始输入，非完整枚举）

目录的 `category` 采用稳定前缀，供覆盖分析分组，不参与匹配逻辑：

- `game_action.*`：摸牌、出牌、吃、碰、杠、胡、流局。
- `game_phase.*`：开局、连胜、连败、终局、等待。
- `player_behavior.*`：快速出牌、长时间思考、重复犹豫。
- `relationship.*`：竞争、打趣、安慰、嘴硬、起哄。
- `emotion.*`：高兴、得意、紧张、失望、生气、无奈。
- `dialogue.*`：接话、反问、调侃、纠正、沉默。
- `continuity.*`：上一手、上一局、熟悉度变化。
- `anomaly.*`：事件过期、资源缺失、对话被打断、模型不可用。

分类是**发现新场景的组织工具**，不是匹配键；禁止用
`人格 × 情绪 × 事件` 的笛卡尔积制造虚假覆盖（见
[EVALUATION-PLAN.md](EVALUATION-PLAN.md) §2）。

## 4. 情绪与心理状态模型

### 4.1 概念分层

| 概念 | 所有者 | v1 实现 |
|---|---|---|
| Stable Personality | `CharacterDefinition.traits`（`talkative/teasing/composure/competitiveness`，[0,1]） | 是，随包固定 |
| Current Emotion | `CharacterRuntimeState.emotion`：`{type, intensity}`，type ∈ `neutral/amused/pleased/annoyed/resigned/smug`，intensity ∈ [0,1] | 是，仅 EmotionReducer 更新 |
| Mood / Short-term Affect | 概念上＝跨多手的慢变量（如连败后的低落） | **v1 不单独存储**：由记忆中的连败/连胜事实 + 情绪规则组合表达；有实验证据再单列 |
| Relationship | `RelationshipState{familiarity, affinity}`（int16，按座） | 是，确定性更新；跨局持久仅在用户开启跨局记忆时 |
| Memory | `CharacterMemory`：本人相关 cue 环（≤8）+ 公共事件环（≤16）+ 已确认上一手摘要 + 连胜败事实 | 是，冻结投影给提示词 |
| Dialogue Intent | DialogueAct 标签（反应与台词都带） | 是，仅作过滤/多样性/评测，不是自由类别 |

### 4.2 EmotionReducer 契约

- 纯函数：`(EmotionState, RelationshipState, BehaviorContext, reaction.emotion_rule) -> 新状态`；同输入同输出，无时钟、无 I/O、无模型。
- 情绪规则来自行为包（场外大模型可**提议**，人工审核后编译为数据），形如
  `{emotion, intensity_delta, cap}`；对**所有**匹配观察者生效，与是否发
  言无关（沉默时情绪照样演化）。
- 衰减：每次场景匹配结算后 `intensity ×= decay`，换手结算时归向
  `neutral`；数值是目录/包声明的常量，不隐式。
- 上限：单一情绪 intensity ∈ [0,1]，被 `cap` 截断；情绪**永不**影响合法
  动作、计分或回合，只影响候选过滤、权重与演出。
- 情绪不是 LLM 生成的随机标签：任何“模型直接决定情绪”的实现都违反本契
  约。

## 5. 时间语义（等待类触发）

- **产生者**：app 控制器（表现/社交层），不是麻将核心。核心不感知玩家
  “思考超时”。
- **时钟**：社交层持有可注入时钟（`IClock`）。产品读墙钟单调时间；测试
  与回放使用可控时钟。麻将权威事件序列不含 SocialCue。
- **权威性**：SocialCue 不属于权威游戏记录；回放麻将牌局不需要它。需要
  复现演出时，回放的是**已记录的 cue 事件**（`CueRaised{scenario_id,
  at_tick, scope}`），与 P1/P2 “只重放已播 line ID”的思路一致。
- **失效**：cue 与后续一切产物绑定
  `match/hand/event/cue/presentation_epoch`；换局、暂停、退出、角色替换
  分配新 epoch，旧字幕/语音/口型零残留。
- 一致性：同一（事件序列 + 已记录 cue 序列 + 包版本 + seed）在测试与回
  放中产生相同输出。

## 6. 发言节奏与防循环（SocialDirector 契约）

- 每桌同一时刻至多一条口播；场景 `priority` 决定抢占（`critical` 牌局
  cue 可打断闲聊，反之不可）。
- 双冷却：单角色冷却（初始 6s 量级）+ 场景级冷却；目录给默认值，听测调
  整。
- **沉默一等**：`silence_policy=allow/prefer` 时，无合格候选、冷却中、
  节奏不允许 → 沉默；`must_speak` 仅用于极少数兜底场景（如教程），且必
  须有中性内置台词。
- **一次回应链**：一条已交付台词最多触发一条带 `exchangeId` 的回应
  （`max_followups ≤ 1`），回应不再触发回应；NPC 不得对自己的发言递归
  生成新话题。
- `recent_delivered` 仅在 `DeliveredSpeech`（实际交付回执）后更新，模型
  只是“选中”不算数。

## 7. 运行时策略阶梯与十问

策略阶梯（每级过评测门才进下一级）：

| 级 | 策略 | 状态 |
|---|---|---|
| 0 | A 固定轮转 | P2 已证，保留为对照基线 |
| 1 | B 加权选择 | **默认产品策略** |
| 2 | D 小模型对候选台词排序/选择 | 首个模型实验（受约束 ID 输出） |
| 3 | C 小模型选 DialogueAct 再确定性选行 | 候选集过大、D 不稳定时的中间层 |
| 4 | E 受约束生成短句 | 远期实验，默认关闭 |
| 5 | F 蒸馏/微调 | 本轮不做，见 ADR Revisit Triggers |

十问：

1. **大模型扩场景后，小模型有效选择空间是否真的增加？** 只有当新场景
   在真实轨迹中可触发且候选 ≥2 时才算增加；“名义覆盖”不算（见评估计
   划的三级覆盖）。
2. **场景增加会不会让选择更难？** 会——这正是默认策略停留在加权、模型
   只在**已过滤小候选集**内工作的原因；候选集构造（人格/情绪/记忆/近期
   去重过滤）承担了绝大多数缩减。
3. **选 DialogueAct 还是 LineId 更稳定？** LineId 输出空间更小且可被
   `OutputShape.allowed_ids` 硬约束、可完整校验；DialogueAct 隐藏了具体
   台词内容，牺牲对重复/人格贴合的判断。**v1 裁决：直接选 LineId**；
   DialogueAct 留作候选集过大时的两段式降级。
4. **如何按情绪与记忆选台词？** 反应匹配（traits 带 + 情绪/记忆守卫）
   → 候选集过滤（情绪区间、locale、声线可用、近期已交付剔除）→ 策略
   在剩余集合内选择。模型只做最后一步。
5. **如何防每局重复同一套台词？** 近期窗口（按实际交付）+ 行权重随机
   化 + 场景级冷却；覆盖缺口由场外流水线补内容而不是靠模型复述。
6. **没有适用候选怎么办？** 沉默优先；`must_speak` 场景与关键反馈用应
   用内置中性台词；禁止模型自由生成兜底。
7. **什么时候保持沉默？** 见 §6：冷却、节奏占用、`prefer` 策略、候选为
   空、或反应声明为 `silent`（叹气/表情不说话也是演出）。
8. **如何防两个 NPC 无限对话？** 一次回应链 + 双冷却 + 同一 cue 至多一
   次发言 + `exchange_followup` 触发不允许嵌套。
9. **如何记录模型选择与确定性策略的质量差异？** 三臂（fixed/weighted/
   model）同夹具同 seed 运行，逐事件记录 offered/delivered/intentional
   silence/fallback/expired/rejected 与唯一台词数、近期重复率；质量由盲
   测对照（评估计划 §4）判定，不以模型自评为准。
10. **什么时候才值得蒸馏/微调？** 仅当（a）教师模型产出的内容在真实游
    戏中盲测显著优于确定性臂，且（b）端侧排序实验显示 0.3B/0.6B 选择质
    量是瓶颈时。届时单独制定 Dataset Contract、数据许可与评测方案。

## 8. SocialDirector 为什么不会长成巨型类

场景数量增长落在**数据**（目录条目、行为包反应），不落在 SocialDirector
代码：它只实现节奏语义（优先级、冷却、一次回应链、epoch），这些语义在
场景从 10 条到 1000 条时不变。新增能力的三条受控通道：新 trigger/guard
kind（纯谓词 + Schema 版本）、新 DialogueAct 枚举值、新演出手势枚举值。
除此之外 SocialDirector 不接受场景特定分支；若某场景需要新节奏语义，先
改契约文档再改代码，不允许在目录里夹带逻辑。
