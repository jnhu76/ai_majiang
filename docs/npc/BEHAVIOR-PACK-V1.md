# BehaviorPack v1 数据契约 — 草案

归属：[ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。本文定
义角色行为包的结构、校验与迁移。机器可读示例见
[examples/](examples/)；示例中的角色与台词**仅用于 Schema 说明**，不是产
品内容。

## 1. 三层内容模型

| 层 | 工件 | 所有者 | 内容 |
|---|---|---|---|
| 场景词汇 | Scenario Catalog（`scenario-catalog.v1.json`） | app，随版本发布 | typed trigger、guards、priority、cooldown、silence_policy、observer_filter |
| 角色反应 | BehaviorPack v1（每角色一份 JSON） | 角色作者/官方流水线 | 按 `scenario_id` 的反应 + 内嵌台词库 |
| 角色身份 | CharacterPack（评审 ADR-06 的 manifest） | 角色作者 | 身份、SOUL、traits、声音、形象、动画 |

## 2. 独立工件 vs 嵌入 CharacterPack（裁决）

| 维度 | 独立 BehaviorPack 工件（**采用**） | behavior 文件嵌入 CharacterPack |
|---|---|---|
| 跨包覆盖/重复分析 | 可静态分析：解析 N 份包即可回答“场景 X 谁覆盖了” | 场景定义分散在各角色包，易漂移 |
| 第三方角色包 | 只需引用共享目录词汇，安装时校验引用 | 每个作者自行复制场景定义，版本漂移 |
| 独立演进/替换 | 可单独版本化、内容哈希 pin、热更新 | 与角色身份耦合，换行为＝换角色包 |
| 运行时复杂度 | 安装期解析一次 `(pack_id, version, content_hash)` 引用 | 无需解析（但也没有上述能力） |
| 代价 | 需要 CharacterPack 声明唯一引用 + 安装期校验 | 无 |

**裁决**：BehaviorPack 是独立版本化工件；CharacterPack 以
`(pack_id, version, content_hash)` **精确引用恰好一份**，安装期解析、运
行时按局 pin——这是“有引用、无依赖图”的折中，**不建跨包依赖管理器**。
同一时刻每座解析为恰好一个 `(CharacterPack, BehaviorPack)` 快照。

引用校验策略：官方包中未解析的 `scenario_id` 一律构建失败；用户导入包中
未解析的 `scenario_id` 降级为**惰性条目**（永不匹配、安装报告列明），保
持 ADR-06 的向后兼容姿态。

## 3. JSON Schema 草案

### 3.1 场景目录条目

```json
{
  "scenario_id": "player.idle.long",
  "title": "玩家长时间未出牌",
  "category": "player_behavior.waiting",
  "trigger": { "kind": "player_idle", "duration_band": "long" },
  "guards": [
    { "kind": "game_active", "expect": true },
    { "kind": "speaker_not_current_player", "expect": true }
  ],
  "priority": "normal",
  "cooldown": { "per_scenario_s": 90, "per_speaker_s": 6 },
  "silence_policy": "allow",
  "max_followups": 1,
  "observer_filter": "all_npc_except_current_player",
  "notes": "等待触发由表现层产生；见 SCENARIO-MODEL.md §5"
}
```

必填：`scenario_id/title/category/trigger/priority/cooldown/
silence_policy/max_followups/observer_filter`；`guards` 可选。字段语义见
[SCENARIO-MODEL.md](SCENARIO-MODEL.md) §2。

### 3.2 BehaviorPack

```json
{
  "schema_version": 1,
  "pack_id": "org.aimj.demo.behavior.jiang-xiaokuai",
  "catalog": { "min_revision": "2026.11.0" },
  "character_ref": { "pack_id": "org.aimj.demo.jiang-xiaokuai", "character_id": "jiang-xiaokuai" },
  "lines": [ /* 台词库，见 §4 */ ],
  "scenarios": [
    {
      "scenario_id": "player.idle.long",
      "reactions": [
        {
          "reaction_id": "jj_idle_impatient",
          "match": { "trait_bands": { "composure": [0.0, 0.45] } },
          "emotion_rule": { "emotion": "annoyed", "intensity_delta": 0.3, "cap": 0.9 },
          "delivery": "speak",
          "dialogue_act": "urge_player",
          "gesture_hint": "react",
          "lines": ["jj_idle_urge_01", "jj_idle_urge_02"]
        },
        {
          "reaction_id": "jj_idle_silent_huff",
          "match": { "trait_bands": { "talkative": [0.0, 0.35] } },
          "emotion_rule": { "emotion": "annoyed", "intensity_delta": 0.2 },
          "delivery": "silent",
          "gesture_hint": "react"
        }
      ]
    }
  ]
}
```

### 3.3 emotion / gesture / dialogue_act 的位置（裁决）

**不**把三者放进同一个结构（需求示例的写法），拆为：

- `emotion_rule` 在**反应**上：情绪演化对**所有**匹配观察者生效，与是否
  发言无关；沉默角色也要生气。
- `dialogue_act` 在**反应**上、并冗余标注在每条**台词**上：反应决定想表
  达什么；台词标签用于过滤、多样性约束与评测统计（缺失或冲突时以反应为
  准并报校验警告）。
- `gesture_hint` 在反应上（沉默演出也需要表情/动作），**台词可覆盖**
  （`line.gesture`）：某句台词自带固定动作（如“哼”配摆手）。
- 台词正文、声音、clip 属于**台词库条目**（资产层），不进反应结构。

理由：同一反应映射多条台词形成变化；情绪与发言解耦；资产与逻辑分离；
校验器可以分层检查。

### 3.4 反应匹配语义

- 反应按声明顺序匹配，首个 `match` 命中者生效（每观察者至多一条）；
  `trait_bands` 是对 `[0,1]` traits 的闭区间谓词（v1 用区间而非命名
  persona，保持 CharacterDefinition.traits 单一事实源；可加
  `persona_hint` 作纯文档注记）。
- `match.observer_scope` 把反应限定到与事件角色的关系，v1 封闭枚举：
  `any`（默认）/ `event_actor`（动作发起者，如碰牌者）/ `event_target`
  （动作承受者，如被碰的舍牌者）/ `others`。目录的 `observer_filter` 先
  选出候选观察者集合，反应再用 `observer_scope` 细分。
- 反应可附加 `guards`（与目录守卫同词汇）表达情绪/记忆/关系条件，如
  “同一座位本手已从我处碰牌 ≥2 次”。
- 反应可携带 `relationship_rule`（声明式、确定性）：对事件角色引用
  （`claimer`/`actor` 等）做 `familiarity_delta`/`affinity_delta` 小步
  更新，写入 `CharacterRuntimeState.relationships`；与情绪一样**不是**模
  型输出。
- 无命中 → 无情绪变化 + 按 `silence_policy` 沉默（或中性兜底）。

## 4. 台词库条目

```json
{
  "id": "jj_idle_urge_01",
  "dialogue_act": "urge_player",
  "locale": "zh-Hans",
  "text": "出牌呀，我茶都续第二回了。",
  "voice": { "voice_id": "jj-bright", "clip": "jj_idle_urge_01", "audio_sha256": "…" },
  "gesture": "react",
  "slots": [],
  "weight": 2,
  "tags": ["idle", "urge"]
}
```

- `text` 为已审核原文；带事实槽位的行用 `slots`（只能引用公开事实格式化
  器，如 `{tile}`、`{score_delta}`），与 P2 的关键词校验同思路。
- `voice.clip` 缺失（无预制音频）时按能力降级：仅字幕或 TTS；声线映射
  由 `voice_id → VoiceProfile → provider SID`，沿用评审 ADR-03/04 的边界。
- 权重与近期窗口构成加权选择；`recent_delivered` 只记实际交付。
- DialogueAct v1 封闭枚举：`silence, urge_player, tease, console, praise,
  brag, grumble, callout, congratulate, vow, deflect, question`。
- 手势 v1 复用评审 ADR-05 身体动作词汇：`idle, talk, draw, discard,
  claim, win, react`；表情由更新后的 `EmotionState` 驱动，不单独声明。

## 5. 校验

**安装期**（`ContentPackManager`，评审 ADR-06 同一边界）：Schema 校验；
`scenario_id` 引用解析（官方严格/用户惰性）；台词 ID 唯一且被引用即存在；
`dialogue_act/gesture/emotion` 枚举合法；traits 区间合法；音频/clip 资源
存在且哈希匹配；locale 完整；许可字段存在；条目数与体积上限。

**投递期**（ValidatedLine 构造）：候选 ID ∈ 本次提供集合；人格/声线/作
用域（`match/hand/event/cue/epoch`）复核；槽位格式化后复核；失败 → 沉
默或中性兜底，绝不发布未验证文本。

## 6. P2 银行迁移与向后兼容

- 现有 `voice::Director` 的 `(event_type, fact_key, speaker)` 精确匹配
  与 P2 TSV 银行是本契约的**前身**：离线转换器把 TSV 行映射为
  `domain_event` 触发的场景条目 + 台词库条目
  （`line_id→id`、`persona_id→traits 带`（经映射表）、`tts_sid→
  voice_id`、`weight` 原样保留）。
- 迁移期内 P2 CLI 与夹具**原样保留**可复现；产品路径切换到 BehaviorPack
  后，P2 的 fixed/weighted 继续作为对照基线。**不并行维护两套产品台词选
  择状态**：`voice::Director` 的轮转/近期逻辑并入新的确定性
  `IDialoguePolicy` 实现后，旧接口仅保留给实验探针。
- 已归档 P1/P2 实验目录（`docs/experiments/2026-11-*`）不因本契约修改。

## 7. 明确不做（v1）

不做跨包依赖管理器、条件脚本、行为树、外部引擎回调；不支持行为包引用其
他行为包；不做多语言自动翻译（locale 各自审核）；不做运行时热编辑。
