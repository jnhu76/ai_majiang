# BehaviorPack v1 数据契约

归属 [ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)。本契约
定义首个 Headless Golden 切片的内容格式。[示例](examples/)用虚构角色，
供静态验证和未来夹具使用。

## 1. 内容所有权

| 工件 | 所有者与内容 |
|---|---|
| Scenario Catalog | app 随版本提供共享场景词汇、触发器、守卫及发言调度参数。 |
| BehaviorPack | 独立版本化的角色反应和台词库。场景用 `scenario_id` 引用。 |
| CharacterPack | 角色身份、SOUL、traits、声线和已批准演出资源，精确引用一份 BehaviorPack。 |

独立工件支持跨角色覆盖分析与独立更新，代价是安装时必须校验引用。
CharacterPack 与 BehaviorPack 作为一对快照固定到整局。场外 Teacher Model
只能提出内容，经验证和人工批准后才可编入 BehaviorPack。

## 2. 安装与引用

- CharacterPack 对 BehaviorPack 的引用是 `(pack_id, version, content_hash)`，
  哈希为该版本 BehaviorPack JSON 发布文件原始 UTF-8 字节的 SHA-256 小写
  十六进制。BehaviorPack 的 `character_ref` 反向确认
  `(CharacterPack.pack_id, character_id)` 身份。它不参与依赖解析，避免循环哈希。
- 版本和目录修订号按三个非负整数组成的元组比较。安装时当前 Catalog 与
  BehaviorPack 的 `schema_version` 均须为 1，目录修订须 ≥ `min_revision`。
  已发布目录 v1 内，同一场景 ID 的含义须保持兼容。
- 安装期执行 Schema 与语义检查。官方包遇到未知 `scenario_id` 失败，用户
  导入包的未知场景条目标记为惰性并报告。惰性条目仍校验自身结构、包内
  ID、引用和台词语义，跳过需要已知触发器的校验。
- 角色、声线与资产必须解析到同一 CharacterPack 快照。拟议
  `ContentPackManager` 在安装时核验资源存在性、资产字节哈希、包的体积
  预算、授权与批准记录。四个文档示例只使用合成角色引用，未携带可安装包
  或音频资源。

## 3. Schema 和反应顺序

[Draft 2020-12 Schema](npc-v1.schema.json) 提供三种根结构
`scenarioCatalog`、`behaviorPack`、`proposalMetadata`。它规定必填字段、
封闭枚举、数值边界及数量上限，未知字段拒绝。JSON 重复成员名拒绝。
日期格式验证需要开启 `date-time` 格式检查。触发器和守卫的执行语义见
[Scenario Model](SCENARIO-MODEL.md)。

- 目录 `scenario_id` 唯一。包内 `scenario_id`、`reaction_id` 和
  LineId 分别在其所属集合内唯一。反应的 LineId 必须在同一包内解析。
- 对每个观察者按 JSON 数组顺序取第一个满足 `match` 的反应。多个 trait
  带、作用域及守卫是 AND，带端点都包含，`lower ≤ upper`。无条件反应如
  `match: {}` 只能在末尾。场景的 `priority` 只控制 SocialDirector
  的调度，不重排匹配。第一条命中后，即使该反应沉默或被冷却，也不试下一条。
- 目录的 `observer_filter` 先选候选观察者，再判断反应的
  `observer_scope`。缺省 `any` 表示候选全集。`event_actor`、
  `event_target` 分别选中角色位，`others` 排除两者中存在的座位。
  v1 只有碰牌触发器提供 target，其他触发器引用 target 属安装错误。
- `delivery=silent` 携带演出提示及可选状态规则。`speak` 必须声明
  DialogueAct 及非空 LineId 集，反应与每条引用台词的 `dialogue_act`
  必须完全相等，否则校验失败。Gesture 默认取反应的 `gesture_hint`，
  台词可用 `gesture` 覆盖。情绪和关系更新遵循 Scenario Model §4。
- DialoguePolicy 只接收已过滤的候选。首个切片按反应中 LineId 顺序构建候
  选，过滤无效公开事实，剔除该座最近实际交付的一条 LineId（若剔除后为
  空则保留原集合）。空集返回 Silence；单候选直接选中。多个候选由加权策略
  按 `weight` 选择。用夹具固定 seed、候选顺序和 RNG 消费次序。模型选择
  只允许本次传入的候选 ID，失效时丢弃，模型失败且仍有效时走同一集合的
  确定性选择。

## 4. 台词、槽位与音频

台词的 `text` 是已审核的静态原文或模板。首个切片支持 `zh-Hans`。占位符
用 `{name}`，正文占位符集合须与 `slots` 精确相等，允许同一槽重复出现，
拒绝未解析花括号。所有值取自触发时已确认且绑定同一作用域的公开快照。

| 槽位 | Formatter | 输入与文本格式 |
|---|---|---|
| `claim_count` | `hand_claim_count_by_seat` | 本手 actor 从 event_target 的舍牌完成的碰牌次数，包含当前事件。正整数用 ASCII 十进制，无前导零。 |
| `tile` | `tile_name` | 当前已公开的碰牌牌面。ID 0–26 顺序为一至九万、筒、条，27–33 为东风、南风、西风、北风、红中、发财、白板。 |
| `streak` | `win_streak` | 截至上一手的玩家连续获胜数，正整数用 ASCII 十进制，无前导零。 |

| `voice.audio` | 安装期规则 | 投递期规则 |
|---|---|---|
| 省略 | 审核静态文案或模板。 | 发布同作用域的已验证字幕。 |
| `mode: clip` | 只用于零槽位台词。用 `clip`、`audio_sha256`、声线和审核原文绑定 WAV。 | 仅可播放绑定原文的已验证 WAV。资源不可用则发布同一句字幕。 |
| `mode: variants` | 至少有一个槽。每项声明完整、唯一的原始类型 `bindings`、渲染后的审核 `text`、`clip` 和 `audio_sha256`。安装校验模板渲染等于该项文本，人工批准录音与文本、语言及声线相符。 | 用全部真实槽值精确匹配唯一变体，且变体原文等于本次渲染才可播放。没有精确匹配或音频不可用时，仅发布同一渲染结果的字幕。 |
| `mode: tts` | 审核原文、槽位来源和声线映射。 | 本地 TTS 使用已验证字幕的同一文本。TTS 不可用、失败或超出语音预算时，在仍有效的作用域内仅发布同一句字幕。 |

安装时每个 clip ID 在所属 CharacterPack 内只能绑定一组资源哈希、审核文本
及声线。同一资源哈希不能绑定不同文字或声线。资产校验确认字节，语音内容
的准确性由人工听审确认。严禁用其他绑定值的 WAV 补位。

投递前核验已提供的 LineId、角色、声线、有效作用域、槽值完整性和音频绑定。
无法从公开事实得出字幕时静默，作用域过期时丢弃字幕及音频。仅实际投递
更新 `recent_delivered`。Schema 和离线脚本只验证静态形状与引用，运行时
投递、实际音频、角色安装和资产校验留待后续实现。

## 5. 离线验证

在预装 `jsonschema` 4.x 的开发环境，从仓库根目录运行：

```sh
python docs/npc/validate_examples.py --self-test
```

脚本离线校验四份 JSON、合成角色/声线引用、目录/包/反应/台词引用及音频
绑定元数据。静态反例覆盖错误字段/枚举/参数、错误引用、被无条件反应遮蔽
的顺序、不可达回应和音频不一致。C++ 核心构建无需 Python。

## 6. P2 银行迁移

现有 `voice::Director` 使用 `(event_type, fact_key, speaker)` 精确查找 P2
TSV 银行。未来离线转换器可保留 `line_id` 和 `weight`，根据经核实的公开
事件映射、角色 traits 和 `tts_sid → voice_id` 映射创建正式行为包。资产
迁移时保留经审核的原文与 WAV 绑定。v1 词汇未覆盖的 P2 行留在实验银行。
P2 CLI、银行、夹具及 P1/P2 归档仍供复现实验，fixed/weighted 用作对照。
