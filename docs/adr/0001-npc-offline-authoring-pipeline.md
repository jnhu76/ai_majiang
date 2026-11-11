# ADR-0001: NPC Offline Authoring and Runtime Behavior Separation

NPC 场外大模型创作与场内确定性执行分离（Offline Teacher–Student NPC
Architecture）

- **Status**: Accepted（2026-11-11）
- **决策范围**: 本状态只批准架构原则与数据契约方向。`docs/npc/` 中的
  Schema 是 v1 契约草案；对应运行时模块、内容编译器与设备验证**均未实现**。
- **评审基线**: 独立架构评审报告（2026-11-11，审计 `29e6c5c`，外部交付物）；
  本 ADR 落盘基线 `dev@5db26bd`。
- **关系**: 扩展评审基线 ADR-04（确定性社交导演与受约束对话）与 ADR-06
  （声明式内容与离线资源），不与其冲突；不取代任何已落盘决策。

## Context

当前 NPC 链路（P1 对话实验、P2 语音循环）已经证明公开事实投影、台词银行、
声线校验与确定性轮转可以安全交付，但暴露的弱点不是推理性能，而是内容与
结构：

1. 人格不稳定：同一事件下三个角色缺乏可辨识的差异化反应。
2. 对话重复：P2 中 17/43 个交付事件只有 1 个候选台词；LLM 臂相对
   fixed/weighted 未显示多样性收益。
3. 情绪没有真实的变化过程：只有一个事件级 `emotion` 标签，无状态演化。
4. 无记忆：NPC 不记得上一手牌和上一段对话。
5. 角色之间无有意义互动：没有接话、打趣、安慰等多角色链。
6. 预制台词覆盖不足：独立开发者难以手工枚举人格 × 场景 × 关系 × 情绪的
   组合。
7. 端侧 0.3B/0.6B 模型难以稳定产生有趣且事实正确的内容（P1 交互 s3 的
   12 次成功产出使用同一句台词）。
8. 人力产能瓶颈：手工扩写银行不可扩展。

证据：`docs/experiments/2026-11-11-p1-dialogue/REPORT.md`、
`docs/experiments/2026-11-11-p2-voice-loop/REPORT.md`。

同时产品约束不变：默认完全离线、无云端 API、规则权威不可被模型触碰
（`AGENTS.md`、`docs/ARCHITECTURE.md`、评审 ADR-01/03/04/06）。

结论：需要一类**只在开发阶段存在**的强创作能力（GPT-6 级场外大模型 +
人工审核），把它的产出编译为**声明式、可校验、可版本化**的内容包；游戏
运行时只消费这些数据包，端侧小模型只是可选的候选排序层。

## Decision

系统划分为三条相互隔离的路径。**大模型是开发工具依赖，不是游戏运行时
依赖。**

### A. Authoring Path（场外创作，开发期）

允许使用 GPT-6 等大型语言模型、人类编剧、规则模板与自动校验工具，运行在
开发工作站或 CI 离线任务中。职责：发现场景、扩充事件/情绪/关系组合、为人
格推演行为、创作候选台词与多角色对话、提出情绪转移建议、发现现有库的重
复与缺口、创建测试与反事实用例。

约束：

- 产出只是 **Proposal（提案）**，必须经过 Schema/语义校验、覆盖与重复分
  析、人工审核后才能进入正式行为包；不存在绕过审核的直通路径。
- 场外大模型不得修改麻将规则、牌墙、计分或任何权威状态。
- 五个逻辑角色（Scene Explorer / Character Behavior Designer / Dialogue
  Writer / Coverage Critic / Quality Reviewer）用**同一模型 + 不同提示词
  模板**承担，不引入多 Agent 编排框架、长期运行服务或数据库。
- 契约见 [`docs/npc/AUTHORING-PIPELINE.md`](../npc/AUTHORING-PIPELINE.md)；
  提案元数据与运行时包分开存储，提案库不进入移动端应用。

### B. Runtime Path（场内执行，玩家设备）

运行时输入只来自：公开麻将事件投影、CharacterDefinition、
CharacterRuntimeState、Scenario Catalog、NPC Behavior Pack。数据流：

```text
PublicGameEvent / SocialCue
      |
      v
ScenarioMatcher（场景目录，声明式）
      |
      v
BehaviorContext（冻结的公开事实 + 心理状态投影）
      |
      v
EmotionReducer（确定性规则）──> CharacterRuntimeState
      |
      v
SocialDirector（发言节奏权威：优先级/冷却/沉默/一次回应）
      |
      v
IDialoguePolicy（候选选择：默认确定性加权；可选端侧模型排序）
      |
      v
ValidatedLine + PresentationIntent
```

核心裁决：

1. **情绪是确定性规则计算，不是模型输出。** EmotionReducer 是纯函数，
   规则以声明式数据随行为包分发（场外大模型可以提议规则，审核后编译为
   数据）。端侧模型永远不设置情绪。
2. **端侧模型第一阶段只做“对已提供候选 LineId 的受约束选择/排序”**，
   沿用评审 ADR-04 的“只返回合法候选 ID”契约与 `OutputShape.allowed_ids`
   机制。DialogueAct 是台词与反应的语义标签（用于过滤、多样性约束与评
   测），暂不作为模型的第一输出层；受约束短句生成仅保留为实验选项，默
   认关闭。
3. **运行时策略阶梯**（自上而下逐步引入，每级都要过评测门）：
   确定性轮转（现状，P2 已证）→ 加权选择（默认）→ 小模型候选排序（可
   选实验）→ 受约束生成（远期实验）。禁止跳级默认开启。
4. **沉默是一等结果。** 场景可声明沉默策略；无合格候选、冷却中、节奏不
   允许时都走沉默或中性兜底，不强行发言。
5. **防无限对话**：每条发言最多触发一次带 `exchangeId` 的一次性回应；
   NPC 不能对自己的发言递归生成新话题；发言与全局双冷却由 SocialDirector
   强制。
6. **等待类社交触发（如玩家 20 秒未出牌）属于表现/社交层**，由 app 控制
   器经可注入时钟产生 SocialCue，不写入麻将权威记录；测试与回放使用可控
   时钟 + 已记录的 cue 事件。见
   [`docs/npc/SCENARIO-MODEL.md`](../npc/SCENARIO-MODEL.md) §5。

### C. Evaluation & Feedback Path（评测反馈）

区分五类证据：自动程序验证、LLM 辅助评审（仅辅助，不作为独立事实证
据）、人工质量评审、真实游戏测试、已批准的正式内容。评测可以产生新的创
作任务，但生成结果必须重新走审核。游戏遥测默认仅存本地；外部提交是明确
授权的可选操作且先处理私人信息。契约见
[`docs/npc/EVALUATION-PLAN.md`](../npc/EVALUATION-PLAN.md)。

### 数据契约（摘要）

三层内容模型，详细 Schema 见
[`docs/npc/BEHAVIOR-PACK-V1.md`](../npc/BEHAVIOR-PACK-V1.md)：

- **Scenario Catalog**：app 持有的共享场景词汇表（typed trigger + 声明
  式 guard + 优先级 + 冷却 + 沉默策略），随应用版本化。
- **BehaviorPack v1**：每角色一份，按 `scenario_id` 声明反应（人格特质
  带、情绪规则、DialogueAct、台词引用、演出提示）+ 内嵌台词库。
- **CharacterPack**：身份、SOUL、声音、形象、traits；以内容哈希**精确引
  用一份** BehaviorPack，不建跨包依赖管理器。

### 与评审 ADR-04 / ADR-06 的边界

不创建第二套并行 NPC 系统：`SocialDirector` 仍是发言节奏权威；
`CharacterRuntimeState` 仍是情绪与记忆的运行时所有者；`IDialoguePolicy`
仍是对白策略边界；`ITextGenerationBackend` 仍不理解麻将规则；
`ICharacterAnimator` 不理解 LLM Prompt；`IAssetRepository` 不负责创作新
内容；Teacher Model 不参与任何运行时权威计算。现有 `voice::Director` /
P2 银行格式是 BehaviorPack 台词库的迁移来源，见
[`docs/npc/BEHAVIOR-PACK-V1.md`](../npc/BEHAVIOR-PACK-V1.md) §6。

## Alternatives

| 备选 | 裁决 | 理由 |
|---|---|---|
| 运行时调用云端大模型生成台词 | 拒绝 | 违反完全离线产品约束；时延、成本、隐私与事实风险不可接受。 |
| 只靠更强端侧模型（更大本地权重）自由生成 | 拒绝 | 0.3B/0.6B 的事实正确性与趣味性已被 P1/P2 证伪；创作 breadth 需要的最强模型不可能常驻端侧。 |
| 纯手工扩写台词银行，不建创作流水线 | 拒绝 | 独立开发者产能不足；覆盖缺口（17/43 单候选）已现；无覆盖度量则无法定位缺口。 |
| 运行时自由生成 + 事后过滤 | 拒绝 | 任意自然语言无法靠关键词完全验真（评审 ADR-04 已裁决）；验证成本与事实风险高。 |
| behavior 文件直接嵌入 CharacterPack、无独立工件 | 拒绝 | 无法对场景覆盖/重复做跨包静态分析，第三方角色包会各自漂移场景定义；代价是引用校验，收益是单一可校验工件。对比见 [`docs/npc/BEHAVIOR-PACK-V1.md`](../npc/BEHAVIOR-PACK-V1.md) §2。 |
| 多 Agent 创作编排框架 / 长期服务 / 数据库 | 拒绝 | 五个逻辑角色用提示词模板即可承担；编排复杂度与维护成本不成比例。 |
| 现在就建设蒸馏/微调训练平台 | 拒绝（本轮） | 运行时推理与训练数据生成是两件事；在未证明内容质量收益前不承诺训练路线。Revisit 触发见文末。 |

## Rationale

- 玩家可感知的 NPC 质量（人格辨识、记忆、互动、不重复）主要由**内容覆盖
  与结构**决定，其次才是运行时选择策略。把最强模型放在开发期创作、把确
  定性系统放在运行时兜底，是对两类能力边界的直接对齐。
- P2 已证 fixed/weighted 确定性路径的安全与可复现；本决策在其上加数据
  规模而不是换运行时范式，风险最小。
- 声明式包继承评审 ADR-06 的可安装校验、离线分发与无脚本边界，安全模型
  不变。
- 即使永远不蒸馏/微调小模型，本架构仍然有效：教师模型的产出直接改善确定
  性路径的覆盖与多样性；小模型只是可选增强层。

## Consequences

- 正向：场景覆盖可度量（名义/可触发/实际交付）；台词库可版本化审计；
  第三方角色包有明确契约（场景词汇表）；运行时复杂度不随场景数增长。
- 代价与义务：
  - 必须维护场景目录版本与行为包兼容矩阵；新增 trigger/guard 种类需要
    代码 + Schema 版本共同演进（有意的摩擦）。
  - 必须建设最小离线工具：Schema 校验器、覆盖/重复分析、提案元数据格
    式、Golden 场景运行器（后续独立 PR）。
  - 人工审核是产能瓶颈，流水线必须把人的注意力集中在有争议的提案上。
  - 场外模型输出的情绪/关系规则必须人工审核后编译为数据，不允许自由数
    值模型直接上线。
- 明确不做（本轮）：不重写 `GameSession`、不加 llama.cpp SDK 依赖、不引
  入云 API/Python 运行时依赖、不建 NPC Agent Framework/ECS/行为树引擎/
  插件系统、不批量生成未经审核的正式对白、不提交模型权重与第三方版权素
  材。

## Invariants

1. NPC 输入只含公开事实投影；任何路径不得引入对手暗牌、牌墙顺序或私有
   信息。
2. Teacher Model 只使用授权设计数据或显式标记的合成场景；未确认的胜负、
   分数、胡牌不得以事实发布。
3. 模型（场外或端侧）不得修改牌墙、手牌、合法动作、计分、回合顺序、权
   威状态或其他玩家私有信息；模型提议的情绪变化不得改变合法动作。
4. 角色配置是纯数据：不执行脚本、不覆盖可信指令、不访问文件；`SOUL.md`
   是不可信风格素材，只能以受控定界进入提示词，不作为 system 指令。
5. 对话绑定 `match/hand/event/cue/presentation_epoch`；换局、暂停、退
   出、角色替换后旧输出必须失效。
6. `recent_delivered` 只按**实际交付结果**（`DeliveredSpeech`）更新，不是
   模型选中即更新。
7. 模型只能选择本次请求明确提供的候选 ID（`OutputShape.allowed_ids` 语
   义）；未提供/过期的 ID 一律拒绝并走确定性兜底。
8. 场景触发与情绪演变只依赖公开事件、声明式规则与记录在案的 cue，不依赖
   不可复现的隐含模型状态。
9. 同一输入（事件序列 + 包版本 + 初始状态 + RNG seed）下，无模型路径的
   全部输出（匹配、情绪、意图、候选、演出）逐位可复现。
10. 每桌同一时刻至多一条口播；每次发言至多一次带 `exchangeId` 的回应；
    NPC 不得递归自我触发。

## Security Boundaries

- 场外创作环境的输入白名单：仓库内游戏事件目录、已批准角色包、显式合成
  场景夹具。禁止把真实牌局记录（可能含暗牌）直接喂给场外模型。
- 提示词注入边界：所有用户/第三方内容（SOUL、自定义包文案）在进入任何模
  型提示词时使用可信应用模板定界；模型输出永远不作为指令执行。
- 内容许可：每份提案必须带 `licensing_notes`；不提交未授权或第三方版权
  人物素材；对外分发的包附署名与许可。
- 遥测默认本地；外部提交需显式授权并先做脱敏（见
  [`docs/npc/EVALUATION-PLAN.md`](../npc/EVALUATION-PLAN.md) §6）。

## Integration Points

| 模块 | 本决策下的角色 |
|---|---|
| `GameSession` / 公开投影 | 唯一事实来源；向社交层输出公开事件，不接受社交层写回。 |
| `SocialDirector` | 发言节奏与互动链权威；消费 `SpeechPlan`/`ValidatedLine`，不匹配场景、不算情绪。 |
| `CharacterRuntimeState` | 情绪、关系、记忆、`recent_delivered` 的运行时所有者；只被 EmotionReducer 与交付回执更新。 |
| `ScenarioMatcher` / `EmotionReducer`（新增，后续 PR） | 纯函数组件，输入目录/行为包数据与冻结上下文，无 I/O、无模型。 |
| `IDialoguePolicy` | 候选选择边界；默认确定性加权实现，可选模型排序实现共用同一候选集。 |
| `ITextGenerationBackend` / `ModelManager` | 不理解麻将；只接受消息 + `OutputShape` 约束，返回候选 ID。 |
| `voice::Director` / P2 银行 | BehaviorPack 台词库与确定性选择器的迁移来源，迁移期内不并行维护两套产品台词选择状态。 |
| `ContentPackManager` / `IAssetRepository`（评审 ADR-06） | BehaviorPack 按同一声明式包边界安装、校验、pin 版本。 |
| `ICharacterAnimator` / 表现层 | 只接收 `PresentationIntent`（情绪态 + 手势枚举），不接触提示词。 |
| Axmol app 控制器 | 等待类 SocialCue 的产生者（可注入时钟）与 presentation epoch 的分配者。 |

## Migration

分阶段，均以独立 PR 交付；本轮仅落盘文档与示例。

1. **已批准（本 PR）**：ADR + `docs/npc/` 四份契约 + JSON 示例。
2. **最小验证切片（下一 PR，必须立即开发）**：场景目录 v1 加载器 +
   ScenarioMatcher + EmotionReducer + 确定性加权 `IDialoguePolicy` +
   Golden 场景运行器（headless，无模型），覆盖三个示范场景
   （`player.idle.long`、`game_action.meld.claimed_of_my_discard`、
   `continuity.hand_opening_after_player_win`）。
3. **银行迁移**：离线转换器把 P2 TSV 银行导入 BehaviorPack 台词库；P2
   实验、夹具与报告原样保留可复现。
4. **端侧模型排序实验**：在 `experiment/*` 分支，同一 Golden 夹具上比较
   fixed/weighted/model 三臂；过评测门才进 `dev`。
5. **暂不做**：受约束生成、跨包依赖管理、Mood 独立存储、蒸馏/微调、内
   容目录服务。
6. **需真实实验才决定**：0.3B/0.6B 排序是否稳定优于 weighted；是否需要
   DialogueAct 中间层；是否值得训练专属小模型。

## Acceptance Criteria

本 ADR 自身：

- `docs/npc/` 文档间术语、枚举与示例一致；JSON 示例通过语法校验；所有内
  部链接可达；CI（headless 无模型构建 + CTest）不受影响。

未来实现切片验收（在对应 PR 中逐项给出证据）：

- Golden 场景：固定输入（事件序列、角色档案版本、行为包版本、初始情绪/
  关系/记忆、RNG seed）→ 验证输出（场景匹配、情绪转移、对话意图、选中
  候选、演出意图）完全可复现；模型臂不要求逐字一致，单独实验。
- 覆盖：新增包不得降低既有可触发覆盖率；交付覆盖率有分臂记录。
- 回归：规则行为、旧场景匹配、声线映射、动画动作合法性、离线能力、性能
  预算、过期响应清理不回退（见 [`docs/npc/EVALUATION-PLAN.md`](../npc/EVALUATION-PLAN.md) §5）。

## Revisit Triggers

- 受约束生成在盲测中同时胜过 fixed/weighted 且事实/安全门全过 → 重评
  “实验性受约束生成”是否升级为可选产品策略。
- 场景目录规模使匹配成本或提示词长度成为可测瓶颈 → 引入分级索引或
  DialogueAct 两段式选择。
- 出现真实的跨角色包共享行为/台词需求 → 重评跨包引用（仍限于声明式引
  用，不做脚本）。
- 第二套麻将玩法引入新事件词汇 → 目录按玩法维度分册，Schema 升版。
- 声明式 guard 无法表达已被证实的创作需求，且团队愿意维护沙箱 → 单开脚
  本能力 ADR（沿用评审 ADR-06 的 revisit 条款）。
- 有受控实验证明“教师模型产出的排序/选择训练数据 + 蒸馏小模型”显著优于
  确定性路径 → 单独制定 Dataset Contract、数据许可与评测方案。
