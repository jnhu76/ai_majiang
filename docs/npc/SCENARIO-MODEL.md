# 场景模型（Scenario Model）— v1 契约草案

归属：[ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。
本文定义场内 NPC 的场景/情绪执行模型。目标是**够表达未来互动、但不过度
复杂**：结构放进共享的场景目录，差异放进角色行为包，状态更新全部确定
性。

## 1. 管线与组件职责

以下是 Proposed Interfaces。现有代码入口见 [ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。

```text
PublicGameEvent | SocialCue
  → ScenarioMatcher（目录触发 + 守卫；按观察者选择首条反应）
  → EmotionReducer（计算状态变更）→ CharacterRuntimeState（提交）
  → SocialDirector（发言许可、抢占、冷却、沉默、回应）
  → IDialoguePolicy（许可后的候选选择）→ ValidatedLine + PresentationIntent
```

| 组件 | 所有权 |
|---|---|
| `GameSession` | 权威牌局与已确认的公开事实。公开投影剔除所有暗牌、牌墙顺序和私有状态；NPC 和提示词只读投影。 |
| `ScenarioMatcher` | 目录和行为包的纯匹配。返回场景、观察者和命中的反应，交给状态计算及调度。 |
| `EmotionReducer` / `CharacterRuntimeState` | Reducer 纯计算情绪和关系变更。每座状态拥有者提交变更，持有情绪、关系、记忆和近期交付记录。 |
| `SocialDirector` | 发言的调度、抢占、冷却、沉默、回应配额及 epoch 失效。 |
| `IDialoguePolicy` | 对已许可且已过滤的候选做选择。空集返回 Silence。 |
| `ValidatedLine` | 用绑定的公开快照、声线、槽位及有效期校验最终投递内容。 |

模型故障由推理运行时报告。app 可把故障状态映射成供界面消费的 `SystemCue`
（例如 `backend_unavailable`），并让正在进行的候选选择回退到同一候选集
的确定性策略。超时且作用域已失效时直接丢弃。SystemCue 不进入
ScenarioMatcher、CharacterMemory 或 GameSession，也不分配发言配额。
此路径只需要运行时结果和 app 消费者。

## 2. 场景目录（Scenario Catalog）

由 app 持有并随应用版本化（`catalog_revision`）。BehaviorPack 按 `scenario_id` 引用目录，未知引用的安装策略见
[BehaviorPack §2](BEHAVIOR-PACK-V1.md)。

### 2.1 字段：必填 vs 派生

| 字段 | 归属 | 说明 |
|---|---|---|
| `scenario_id` / `title` / `category` | 目录必填 | 稳定 ID、人类可读名、分类（见 §3） |
| `trigger` | 目录必填 | typed trigger：`kind` + 封闭参数（见 §2.2） |
| `guards` | 目录可选 | 声明式守卫列表，AND 语义（见 §2.3） |
| `priority` | 目录必填 | `low / normal / high / critical`；重要牌局 cue 可抢占闲聊 |
| `cooldown` | 目录必填 | `per_scenario_s` + `per_speaker_s` 默认值 |
| `silence_policy` | 目录必填 | `allow`（初始切片唯一值，见 §6） |
| `max_followups` | 目录必填 | 原始发言的回应配额，取 0 或 1；回应场景固定 0 |
| `observer_filter` | 目录必填 | 候选观察者集合，v1 封闭枚举：`all_npc / all_npc_except_actor / all_npc_except_current_player / actor_only` |
| Actor / Observer | **运行时派生** | 来自有类型的输入与 `observer_filter`（见 §2.2） |
| Personality / Emotion / Relationship / Memory | **运行时派生** | 匹配时从 `CharacterRuntimeState` 取冻结投影 |
| DialogueAct / 台词 / 演出 | **行为包** | 反应层声明，目录不涉及 |

### 2.2 Trigger kinds（v1 封闭枚举）

| kind | 参数和来源 | actor / target |
|---|---|---|
| `domain_event` | GameSession 已确认的公开 `meld_claimed`，固定过滤 `claim_kind=pong` 且舍牌者为 NPC。 | 碰牌者 / 被碰的舍牌者。 |
| `player_idle` | app 产生的 SocialCue；`duration_band=short/medium/long` 精确相等。 | 等待出牌的本地玩家 / 无。 |
| `hand_phase` | GameSession 已确认的公开 `first_draw` 阶段标记，每手一次。 | 首次摸牌者 / 无。 |
| `exchange_followup` | SocialDirector 发出的一次性 SocialCue；`requires_act` 精确匹配原始交付的 DialogueAct。 | 原始发言者 / 无。 |

`player` 永远指本地人类玩家，`current_player` 指输入快照中的当前行动座位。
`observer` 指当前待评估的 NPC。缺失的座位引用无法通过身份比较或关系
查询。目录按序评估过滤器，`actor_only` 只包括 actor 是 NPC 的情形。
场景分类只用于报告。新增 trigger 或 guard 时一起扩展 Schema 和实现。

### 2.3 Guard kinds（v1 封闭枚举）

| kind | 参数及计算 |
|---|---|
| `game_active` | 将快照中的对局活动状态与布尔 `expect` 比较。 |
| `speaker_not_current_player` / `speaker_not_actor` | 将观察者与已知角色座位不相等的结果与 `expect` 比较。角色缺失时守卫失败。 |
| `emotion_in` | 匹配观察者在本次输入处理前的情绪类型，`set` 为封闭情绪枚举的非空子集。 |
| `fact_query` | 仅执行下表中的查询，参数由 Schema 封闭。 |

| query | 参数 | 数据及语义 |
|---|---|---|
| `hand_claim_count_by_seat_min` | `by=actor`、`against=observer`、`min` | 仅用于碰牌事件。本手从该观察者舍牌完成的碰牌次数，包含当前已确认的碰牌。 |
| `last_hand_winner` | `who=player` | 仅用于 `first_draw`。上一手已确认且本地玩家为获胜者之一。 |
| `win_streak_min` | `who=player`、`min` | 仅用于 `first_draw`。截至上一手的连续已确认获胜手数，败局或流局打断。 |
| `relationship_affinity_max` | `toward`、`affinity_max` | 观察者指向指定座位的 affinity ≤ 上限，未建立关系从 0 开始；缺失目标守卫失败。 |

每组守卫使用 AND。公开事实查询用已确认、带作用域的快照，缺失或未确认即失败。
关系查询用观察者在本次输入处理前的冻结状态。

## 3. 场景分类体系（初始输入，非完整枚举）

目录的 `category` 采用稳定前缀，供覆盖分析分组，不参与匹配逻辑：

- `game_action.*`：摸牌、出牌、吃、碰、杠、胡、流局。
- `game_phase.*`：开局、连胜、连败、终局、等待。
- `player_behavior.*`：快速出牌、长时间思考、重复犹豫。
- `relationship.*`：竞争、打趣、安慰、嘴硬、起哄。
- `emotion.*`：高兴、得意、紧张、失望、生气、无奈。
- `dialogue.*`：接话、反问、调侃、纠正、沉默。
- `continuity.*`：上一手、上一局、熟悉度变化。
- `anomaly.*`：评估分类可记录过期、资源缺失、打断和模型不可用，诊断走 app 的 SystemCue 路径。

分类是**发现新场景的组织工具**，不是匹配键；禁止用
`人格 × 情绪 × 事件` 的笛卡尔积制造虚假覆盖（见
[EVALUATION-PLAN.md](EVALUATION-PLAN.md) §2）。

## 4. 情绪与心理状态模型

### 4.1 概念分层

| 概念 | 所有者 | v1 契约 |
|---|---|---|
| Stable Personality | `CharacterDefinition.traits`（`talkative/teasing/composure/competitiveness`，[0,1]） | 是，随包固定 |
| Current Emotion | `CharacterRuntimeState.emotion`：`{type, intensity}`，type ∈ `neutral/amused/pleased/annoyed/resigned/smug`，intensity ∈ [0,1] | 是，由状态所有者提交 Reducer 结果 |
| Mood / Short-term Affect | 概念上＝跨多手的慢变量（如连败后的低落） | **v1 不单独存储**：由记忆中的连败/连胜事实 + 情绪规则组合表达；有实验证据再单列 |
| Relationship | `RelationshipState{familiarity, affinity}`（int16，按座） | 是，确定性更新；跨局持久仅在用户开启跨局记忆时 |
| Memory | `CharacterMemory`：本人相关 cue 环（≤8）+ 公共事件环（≤16）+ 已确认上一手摘要 + 连胜败事实 | 是，冻结投影给提示词 |
| Dialogue Intent | DialogueAct 标签（反应与台词都带） | 是，仅作过滤/多样性/评测，不是自由类别 |

### 4.2 EmotionReducer 契约

1. 按已记录输入序列处理，每个输入先冻结公开事实和所有观察者的状态。
   已确认的当前碰牌计入此时的公开计数。对每座至多选一个场景（同一输入
   以目录声明顺序取第一个满足触发、观察者过滤和目录守卫的条目），然后
   在包中取首条命中反应。无反应保留状态并沉默。
2. Reducer 用该观察者的冻结值纯计算。省略 `emotion_rule` 或显式
   `{emotion: neutral, intensity_delta: 0}` 均保持已有情绪。其余规则
   采用 `clamp(base + intensity_delta, 0, cap)`，cap 缺省为 1；
   情绪类型相同则 base 为旧 intensity，类型变化则 base 为 0。
   结果为 0 时状态规范为 `{type: neutral, intensity: 0}`。
   关系更新只改本观察者指向 `toward` 座位的分量，按 int16 范围饱和。
3. 全部计算结束后，由拥有各座 `CharacterRuntimeState` 的对局线程按座位
   顺序提交。计算只读冻结快照，所以评估观察者的顺序不改变结果。
   显式沉默、冷却、抢占、无合格音频或无有效投递均保留本步提交的变化；
   手势只用提交后的情绪，演出时机仍由 SocialDirector 决定。
4. 初始切片没有基于时间或匹配次数的衰减。每手开始（首次处理该手事件前）
   将各座情绪重置为 neutral/0，清除手内计数和记忆环；上一手已确认摘要、
   连胜事实和同一角色的座位关系保留。交付记录归状态所有者，仅在交付
   回执后更新。状态提交先于发言调度。
5. 整局固定角色包快照。新包随下局激活，重建该座状态（neutral/0、清空关系、
   记忆和近期交付）；第一切片仅保留局内记忆。退出、暂停及切包失效旧
   presentation epoch，不能靠迟到回执写入新状态。

## 5. 时间语义（等待类触发）

- app 控制器使用可注入单调时钟生成 SocialCue。夹具或演出回放直接提供已
  记录的 duration band、单调 tick 和作用域；本契约的首个切片从 cue
  输入开始，等待阈值归 app。
- 绑定 `match/hand/source_id/presentation_epoch`。source_id 区分公开事件
  与社交 cue。换手、换局、暂停、退出或角色包替换后旧作用域产物失效。
  麻将回放读取权威记录，演出回放另外读已记录的 cue。
- 同一有序输入、tick、包与初始状态重放同一场景及状态转移。用当前真实
  时间重新推导 cue 会改变输入。

## 6. 发言节奏与防循环（SocialDirector 契约）

- 每个输入至多许可一条发言，每桌同一时刻至多一个有效发言。按
  `critical > high > normal > low` 选择；同优先级按目录顺序，仍相同按
  NPC 座位升序。新意图仅在严格更高优先级时可打断在播发言。显式沉默的
  反应提交状态和可用手势，但不竞争发言。
- 冷却分别按桌内 scenario ID 与说话座位记时，以实际交付的单调 tick
  起算，经过秒数 ≥ 对应配置才可再交付。当前场景的两个门都须通过。
  `allow` 容许无反应、无合格候选、冷却或占用时保持沉默。显式沉默或空
  候选不会占用冷却。一条发言获许可后，候选为空则本次沉默，未选座位
  不再重排。第一切片以干净的冷却表开始每局。
- `DeliveredSpeech` 指有效作用域中实际发布字幕或接收通过校验的首段口播
  的一次性回执。纯字幕也算交付；仅选中 LineId、过期、失败均不算。回执
  去重后更新近期记录、双冷却并尝试安排回应。
- 只有原始发言实际交付且其场景 `max_followups=1` 才创建 `exchangeId`。
  该交换至多发出一个回应 cue（一次调度即消耗配额，包含被冷却或失效的情况）。
  候选排除原始发言者，回应携同一 ID 并标记为终止。回应 cue 在原始台词
  结束后处理；纯字幕交付在原始回执之后即可处理。重复回执、迟到回执、
  回应回执都不能再创建回应；没有递归或第三句。将来如确需三句互动，应
  单独评审有预先批准台词及固定长度的 ExchangePlan。

## 7. 运行时策略阶梯与十问

策略阶梯（每级过评测门才进下一级）：

| 级 | 策略 | 状态 |
|---|---|---|
| 0 | A 固定轮转 | P2 已证，保留为对照基线 |
| 1 | B 加权选择 | **计划默认策略** |
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
   拟议接口 `OutputShape.allowed_ids` 硬约束、可完整校验；DialogueAct 隐藏了具体
   台词内容，牺牲对重复/人格贴合的判断。**v1 裁决：直接选 LineId**；
   DialogueAct 留作候选集过大时的两段式降级。
4. **如何按情绪与记忆选台词？** 先用 traits 带和情绪/记忆守卫匹配反应，
   经 SocialDirector 许可后，按 locale、公开事实槽位和近期实际交付过滤
   LineId。模型只做最后一步。
5. **如何防每局重复同一套台词？** 近期窗口（按实际交付）+ 行权重随机
   化 + 场景级冷却；覆盖缺口由场外流水线补内容而不是靠模型复述。
6. **没有适用候选怎么办？** 保持沉默。模型排序失败可回退到同一合格候选集的确定性选择。
7. **什么时候保持沉默？** 见 §6：冷却、节奏占用、候选为
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
