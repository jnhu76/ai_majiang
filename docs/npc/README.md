# NPC 行为契约（v1 草案）

状态：**契约草案**。对应 [ADR-0001](../adr/0001-npc-offline-authoring-pipeline.md)
（Accepted）。Schema 与流程尚未实现；本目录定义的是后续实现 PR 必须满足
的契约，以及可离线校验的示例。

| 文档 | 内容 |
|---|---|
| [SCENARIO-MODEL.md](SCENARIO-MODEL.md) | 场景模型：管线、场景目录、触发与守卫、情绪与心理状态、时间语义、节奏与防循环、运行时策略阶梯 |
| [BEHAVIOR-PACK-V1.md](BEHAVIOR-PACK-V1.md) | BehaviorPack v1 数据契约：与 CharacterPack 的关系、JSON Schema、台词库、P2 银行迁移 |
| [AUTHORING-PIPELINE.md](AUTHORING-PIPELINE.md) | 场外大模型创作流水线：流程、教师角色、提案元数据、审核发布 |
| [EVALUATION-PLAN.md](EVALUATION-PLAN.md) | 评估计划：覆盖、质量、回归、Golden 场景语料、隐私 |
| [examples/](examples/) | 机器可读示例：场景目录、行为包、提案元数据（仅文档用途，非安装内容） |

术语速查：Teacher Model＝场外开发期大模型；端侧模型＝玩家设备上的可选
0.3B/0.6B 推理；场景目录（Scenario Catalog）＝app 持有的共享场景词汇
表；BehaviorPack＝角色行为反应包；SocialCue＝表现/社交层产生的社交触发
（如等待超时）。
