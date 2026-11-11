# 评估计划（NPC 内容质量）— v1 契约草案

归属：[ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。原则：
**“生成了 5000 条台词”不是成功指标**；教师模型对自己文本的打分不是独立
事实证据；小样本要如实报告。

## 1. 证据分层

| 层 | 角色 |
|---|---|
| 自动程序验证 | 事实性唯一可自动化证据：Schema、枚举、槽位、覆盖、去重、Golden 重放 |
| LLM 辅助评审 | 仅用于排序人工审核队列与初筛提示，不作为质量结论 |
| 人工质量评审 | 发布门：人格、趣味、许可 |
| 真实游戏测试 | 交付覆盖率、重复率、节奏观感 |
| 已批准正式内容 | 唯一可进入产品的状态 |

证据状态沿用实验约定：`PASS / FAIL / INCONCLUSIVE`；无设备/无听测数据
不得写成通过。

## 2. Coverage（覆盖）

三级口径，逐级收紧，**禁止用 `人格 × 情绪 × 事件` 笛卡尔积做名义分母**：

| 口径 | 定义 | 采集 |
|---|---|---|
| 名义覆盖（nominal） | 目录中存在该 `scenario_id` 且某包声明了反应 | 静态分析 |
| 可触发覆盖（triggerable） | 在受控时钟的脚本化轨迹上，该场景至少匹配一次 | 离线夹具回放 |
| 实际交付覆盖（delivered） | 真实游戏对局中该场景实际交付过台词 | 本地对局日志 |

统计维度：`scenario_id`、trigger kind、人格 traits 带、情绪态、关系态、
多轮互动（`exchange_followup` 交付数）；并跟踪**真实轨迹中的未匹配事
件**（高频者回流给 Scene Explorer，见
[AUTHORING-PIPELINE.md](AUTHORING-PIPELINE.md) §6）。

新包回归门：不得降低既有可触发覆盖率；名义覆盖增长必须伴随可触发交付计
划，否则记为待补测试。

## 3. Quality（质量）

- 人格辨识度：去名字盲选三选一（对照机会水平 33%），报告区间与混淆矩
  阵。
- 情绪合理性：情绪转移是否与公开事实一致（抽检 + 事实违规零容忍）。
- 对话自然性 / 连续性 / 角色间互动质量：人工盲评（成对对照）。
- 语义重复率：同场景近期窗口内近义行命中比例（自动 + 抽检）。
- 事实正确率：带槽位行逐条核对公开事实；已发布关键事实错配为 0 是发布
  阻断项。
- 人工趣味性评分：Likert + 成对偏好，样本量与置信区间如实报告。

盲测协议沿用评审报告 I.1 的方法（同一场景、随机平衡顺序、逐玩家成对对
照；模型臂须同时比较 fixed 与 weighted 基线）。

## 4. Golden Scenario Corpus（最小金标集）

**固定输入**：

```text
有序 PublicGameEvent 与记录的 SocialCue（含单调 tick 与作用域）
Character Profile Revision
Behavior Pack Revision（含场景目录 revision）
Initial Emotion / Initial Relationships / Initial Memory
确定性 RNG seed、初始双冷却及近期交付记录
```

**验证输出**（固定输入下重放一致）：

```text
Scenario Match（scenario_id + 命中反应）
Emotion Transition（前/后状态）
Dialogue Intent（DialogueAct）
Selected Candidate（LineId 或 Silence）
Presentation Intent（情绪态 + 手势）
```

### 首批 Golden 合同用例

以下是给后续 Headless Golden 运行器的测试预期，状态为
**Runtime NOT IMPLEMENTED**。使用目录与两份示例包；本地玩家座位 0，
姜小快座位 1，温阿默座位 2，第三个 NPC 座位 3 不含匹配反应。默认开局
无发言占用、无冷却、近期为空，locale 为 `zh-Hans`，本地 TTS 关闭。
各案例独立。traits 按 CharacterPack 夹具提供：

| 角色 | talkative | teasing | composure | competitiveness |
|---|---:|---:|---:|---:|
| 姜小快 | 0.8 | 0.4 | 0.2 | 0.7 |
| 温阿默 | 0.55 | 0.3 | 0.9 | 0.65 |

1. **`golden.idle`**，`player.idle.long`。活动对局，当前行动者为 0；
   t=100 收到 `long` cue。预置姜小快最近一条为 `jj_idle_urge_02`，
   两角色初始 neutral/0、affinity=0。姜小快先匹配 `jj_idle_impatient`
   并变为 annoyed/0.30，温阿默匹配 `wa_idle_calm_smile` 并变为
   amused/0.20。仅交付姜小快 `jj_idle_urge_01`，DialogueAct 为
   `urge_player`，手势 `react`。确认交付后，同一交换发出一次回应 cue。
   温阿默匹配 `wa_followup_tease_impatient`，变为 amused/0.35，
   对姜小快 familiarity +1，并交付 `wa_idle_tease_impatient_01`
   （`tease`、`talk`）。重复原始回执和回应回执都不得产生第三句。
   变体 t=100 时姜小快仍在冷却，两个原始反应照常更新情绪，交付及回应均为零。
2. **`golden.claim`**，`game_action.meld.claimed_of_my_discard`。
   t=100 公开确认座位 3 碰座位 1 的舍牌，此手双方计数从 1 到 2；
   两角色初始 neutral/0。姜小快匹配 `jj_claim_callout_when_repeated`
   （先于普通抱怨），变为 annoyed/0.40，对座位 3 affinity −1。温阿默
   匹配 `wa_claim_observer_praise`，变为 pleased/0.15。两者的发言优先级
   相同，座位 1 获准，交付 `jj_claim_callout_repeat_01` 的第 2 次字幕
   （`callout`、`react`），无 WAV、无回应。计数改为 1 时姜小快交付
   `jj_claim_letgo_01`，变为 annoyed/0.25。再将舍牌者改为温阿默并预置
   其 amused/0.40，则温阿默命中 `wa_claim_target_silent`，情绪保持；
   姜小快旁观并静默，actor 也没有反应。
3. **`golden.opening`**，`continuity.hand_opening_after_player_win`。
   t=100 公开确认新手首次摸牌，前两手均已确认座位 0 获胜。先完成换手
   重置，保留已确认摘要。姜小快命中 `jj_opening_vow_streak`，变为
   annoyed/0.35；温阿默匹配 `wa_opening_congratulate`，变为 pleased/0.15。
   座位 1 交付 `jj_win_vow_streak_01` 的连赢 2 把字幕（`callout`、
   `react`），无 WAV、无回应。连胜改为 1 时选 `jj_win_vow_01`
   （`vow`），姜小快为 annoyed/0.20；上一手胜负未确认时场景不匹配。

关系与情绪数值按公式断言。浮点表示可使用容差；同一实现按固定输入重放
仍应得到相同结果。上述案例的台词选择可由过滤后唯一候选断言，
加权多候选的跨平台 RNG 复现留在运行器验收。

- 金标集入库 `tests/`（后续 PR），数量从覆盖三个示范场景起步，增长优先
  级：异常场景（过期、缺资源、打断，以及独立 SystemCue 路径的模型不可用）＞ 情绪演化 ＞ 多轮互
  动。
- 端侧模型臂**不要求逐字一致**：在同金标夹具上单独实验，记录
  offered/delivered/intentional_silence/fallback/expired/rejected 分母
  与选择分布，质量由 §3 盲测判定。

## 5. Regression（回归门）

新增/更新行为包不得破坏：

1. 规则行为：金标牌局摘要不变（行为包不触权威，测试证明之）。
2. 旧场景匹配：既有目录条目的匹配与交付不回退。
3. 角色声线映射：`voice_id → VoiceProfile → SID` 零错配。
4. 动画动作合法性：`gesture_hint` 全部落在 ADR-05 词汇内且资源存在。
5. 离线能力：断网无模型完整可玩。
6. 性能预算：无模型主路径 event→可听 p95 与内存不劣化（对照
   `docs/BENCHMARK_PLAN.md`）。
7. 过期响应清理：换局/暂停/退出后旧字幕、语音、口型零残留（epoch 生效）。

## 6. 隐私与遥测

- 对局与选择遥测**默认仅存本地**；任何外部提交是显式授权的可选操作。
- 外发前脱敏：不含手牌、私聊、设备标识；聚合统计优先。
- 教师模型/评审输入白名单见 `AUTHORING-PIPELINE.md` §2；用户素材不经外
  部模型处理，除非单独授权。

## 7. 报告纪律

- 覆盖率注明口径（名义/可触发/交付）与分母；样本量小或零交付不发布
  p95。
- 任何“趣味性提升”的结论必须给出盲测协议、样本与区间；只有模型臂在成
  对对照中显著优于确定性臂（预注册判据）才允许默认开启该策略。
- Mock 延迟/数字不得描述为模型吞吐或真实语音质量（沿用 `AGENTS.md`）。
