# 场外创作流水线（Authoring Pipeline）— v1 契约草案

归属：[ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。
**大模型是开发工具依赖，不是游戏运行时依赖**：本流水线只在开发工作站或
CI 离线任务中运行，产物是提案与经审核的声明式行为包，玩家设备上不存在任
何场外模型调用。

## 1. 流程

```text
Game Event Catalog（场景目录 + 公开事件类型）
        +
Character SOUL / Traits（已批准角色档案）
        +
Existing Behavior Packs（现状基线）
        |
        v
Teacher Model（GPT-6 级场外大模型，提示词模板）
        |
        v
Scenario Proposals（提案，带元数据）
        |
        v
Schema / Semantic Validation（自动：结构 + 枚举 + 槽位 + 事实键）
        |
        v
Coverage & Duplication Analysis（自动：覆盖缺口 + 语义重复）
        |
        v
Human Review（人工：人格、趣味、许可、事实）
        |
        v
Approved Behavior Pack（版本化、内容哈希、可审计）
        |
        v
Offline Native Runtime（随 app / 官方内容包分发）
```

任何一步失败都会产生**新的创作任务**（补缺口、去重、改写），而不是绕过
审核直接进包。

## 2. 教师模型的五个逻辑角色

五个角色是**提示词模板 + 输入/输出契约**，不是五个 Agent；v1 用同一个大
模型、不同模板、人工串联。不引入多 Agent 编排框架、长期运行服务或数据
库；中间产物是带元数据的文件（提案库），最终产物是仓库中的行为包。

| 角色 | 输入 | 输出（提案类型） |
|---|---|---|
| Scene Explorer | 场景目录 + 真实测试轨迹中的未匹配事件清单 + 玩法阶段/动作词汇 | 新 `scenario` 提案（trigger/guards/priority/冷却/沉默策略） |
| Character Behavior Designer | 场景 + 角色 SOUL/traits + 现有反应 | 新 `reaction` 提案（traits 带、情绪规则、DialogueAct、gesture） |
| Dialogue Writer | 反应 + 台词库现状 + 近期已交付样本 | 候选台词提案（含多角色互动对）与 `silent` 演出提案 |
| Coverage Critic | 目录 × 各包反应矩阵 + 交付覆盖率报告 | 缺口清单：缺失的人格带/情绪/关系/多轮分支；近义重复清单 |
| Quality Reviewer | 提案 + 角色档案 + 上下文事实 | 辅助评审意见：人格一致性、语义重复、上下文合理性（**仅辅助证据**） |

使用纪律：

- 输入白名单：仓库内目录/角色档案/已批准包 + **显式标记的合成场景夹具**
  （如 `docs/npc/examples/` 与离线生成的事件序列）。禁止把真实对局记录
  （可能含暗牌的权威日志）直接送入教师模型；需要“真实感”输入时先经公开
  投影脱敏。
- 所有不可信文本（SOUL、用户包文案、既往台词）进入提示词时使用可信应用
  模板定界；教师模型的输出是**数据**，任何指令性内容一律忽略并在校验中
  报错。
- 教师模型对自己的产出打分只用于排序人工审核队列，不作为质量证据（见
  [EVALUATION-PLAN.md](EVALUATION-PLAN.md) §3）。

## 3. 提案元数据与追踪

每份提案带元数据，示例见
[examples/proposal-metadata.v1.json](examples/proposal-metadata.v1.json)：

| 字段 | 说明 |
|---|---|
| `proposal_id` | 全局唯一，`<日期>-<role>-<序号>` |
| `scenario_id` / `target_pack` | 提案归属的场景与目标行为包 |
| `source_event_schema` | 依据的公开事件 Schema 版本 |
| `character_revision` | 依据的角色档案修订 |
| `generator_model` / `generator_prompt_version` / `generated_at` | 生成来源（模型 ID + 模板版本 + 时间），可复现追溯 |
| `teacher_role` | 五角色之一 |
| `review_status` | `draft → schema_validated → semantically_validated → coverage_reviewed → human_reviewed → approved / rejected / superseded` |
| `validation_result` | 自动校验器输出摘要 |
| `content_hash` | 提案内容哈希（与审核记录绑定） |
| `licensing_notes` | 素材来源与许可声明（必填） |
| `human_reviewer` / `review_notes` | 人工审核记录 |
| `supersedes` / `superseded_by` | 修订链 |

**提案库与运行时包分离**：提案（含审阅、去重、评审元数据）保存在开发侧
提案库，**不打包进移动端应用**；应用只携带已批准行为包。仓库中只提交示
例与 Schema，不提交批量生成的未审核台词。

## 4. 自动校验与人工审核

自动（必须全过才进入人工队列）：JSON Schema；枚举与 ID 引用；台词槽位
只引用公开事实格式化器；声线/clip 引用存在；traits 区间合法；与现有库的
精确/近义重复检测（嵌入或规则均可，阈值在工具中固定并记录版本）。

人工（不可省略）：人格一致性与趣味；事实正确性（尤其带槽位的行）；许可
与署名；`emotion_rule` 数值合理性（拒绝无法解释的数值模型）。审核通过才
能合并进行为包并生成新 `pack version + content_hash`。

## 5. 工具形态（v1）

- 一组离线脚本 + 提示词模板 + 校验器，随仓库 `tools/`（后续 PR）分发，
  可在无网络环境对已导出的模型输出重放校验（模板与输出版本化）。
- Python 仅作为离线工具语言（与 `benchmarks/` 现状一致），**不是游戏依
  赖**；核心 C++20 构建不依赖它。
- 不建常驻服务、任务队列系统或数据库；CI 中作为一次性 Job 运行。

## 6. 与评测回路的连接

Coverage Critic 的输入来自 [EVALUATION-PLAN.md](EVALUATION-PLAN.md) 的
覆盖率报告（可触发/实际交付口径）；真实测试中发现的高频未匹配事件自动
生成 Scene Explorer 任务。回路只产生**提案**，全部回到 §1 流程头部。
