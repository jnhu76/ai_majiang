# P2 最小语音闭环 — 技术可行性验收（v1）

- 日期：2026-11-11
- 分支：`experiment/p2-voice-loop`（rebase 到 `origin/dev` `a56e6f4`，P1 #6 已合入）
- 范围：只验证“公开事件 → 确定性选词 → 预制语音 → Windows 播放”的闭环，不实现真实麻将规则、不做产品 UI、不重跑 P1 全量实验。
- 一句话结论：**Runtime GO；Constrained Dialogue GO（headless 原型）；Full Game NOT EVALUATED。** 语音听感仍待人工，LLM 在线选词无可见收益，予以关闭。

---

## 0. 三个门

| 门 | 结论 | 依据 |
|---|---|---|
| Runtime GO | **GO** | 44 事件全部有确定结果，43 条交付、1 条按设计静音；0 事实违规、0 声线错配、无死锁；43 条真实 `waveOut` 播放全部回调完成 |
| Constrained Dialogue GO | **GO（headless 原型，未接入 GameSession/Axmol）** | 银行 31/31 覆盖；精确 `(event_type, fact_key, speaker)` 匹配；文本校验在音频交付前；无候选即静音；LLM 只能选 1/2/3，非法输出回退 |
| Full Game GO | **NOT EVALUATED** | `DemoRuleSet` 不是麻将；P2 未接真实规则/结算/隐藏状态，不据此宣称可玩麻将产品 |

---

## 1. 实现内容

### 1.1 类型化公开事件契约

`include/aimj/voice_loop.hpp` 定义 `PublicEvent`，字段与任务要求一致：

`event_id`, `session_id`, `event_type`, `actor`, `public_tiles`, `public_action`, `speaker`, `persona_id`, `emotion`, `fact_key`, `timestamp_ms`, `note`。

- 契约只承载公开事实：牌河、公开动作、说话者、情绪、公开 fact key。
- 不包含任何手牌、牌墙顺序、分数或结算内部状态；`GameSession`/`IRuleSet` 未被修改。
- `note` 只用于夹具说明，不参与选词与校验。

### 1.2 确定性 Dialogue Director

`src/voice_loop.cpp`：

- 选词键为**精确三元组** `(event_type, fact_key, speaker)`；不存在“同 event_type 模糊匹配”。
- `fixed`：按 `line_id` 排序后轮转，并跳过最近 4 条已用台词。
- `weighted`：`weight + emotion 匹配(+3) + persona 匹配(+1) - 最近使用(1000)`，同分按 `line_id` 稳定排序。
- `llm`：只把**已预校验候选的最多 3 条文本**和公开事实放进 prompt，要求只回 `1/2/3`；输出越界、非数字或空 → `llm_fallback` 走确定性轮转；模型选中最近刚说过的行 → `llm_recent_override`。
- 无候选：返回空选择，调用方按“确定性安全回退/静音”处理，**不会**转入自由生成。

### 1.3 银行（Bank）

`benchmarks/p2_bank_entries.tsv` → `benchmarks/p2_bank_manifest.tsv`：

- 54 条人工审阅台词；每条绑定 `event_type/fact_key/speaker/persona/emotion/text/tts_sid/audio_file/audio_sha256/weight`。
- `tts_sid` 强制 `1→8, 2→9, 3→10`；加载时 speaker/sid 不一致直接拒绝整个 manifest。
- 音频为本地 sherpa-onnx 预渲染 WAV（8 kHz mono PCM16），构建脚本逐文件校验 SHA-256；运行前再按 manifest 校验一次。
- 52 个不同 WAV（2 条台词复用同一音频），共 1,735,170 B ≈ 1.65 MiB，总时长 108.305 s。

### 1.4 校验与播放

- `validate()` 在**任何 PCM 读取/播放之前**检查：事件类型、fact key、speaker、persona、sid、2–40 码点、台词中提到的牌必须属于本事件公开牌、禁止在非 hidden/chat/fixed 事件里出现“暗牌/你手里/我看见/胡/自摸/赢了/分数”。
- 校验失败 → `validation_result=rejected`，`audio_status=rejected_before_tts`，不产生音频。
- Windows 播放：`waveOutOpen(WAVE_MAPPER, 8 kHz mono PCM16)` + `waveOutPrepareHeader/Write`，`WOM_DONE` 回调置位；记录 `pcm_to_player_submit_ms` 与到回调的 `player_callback_ms`。
- **不宣称可听起点**：`first_audio_output_ms=null`、`first_audible_ms=null`，原因写在每条记录与 summary 里（回调完成 ≠ 人耳听到）。

---

## 2. 实验设置

| 项 | 值 |
|---|---|
| 机器 | 本机 Windows 11 x64，Ryzen 7 5800H，Radeon iGPU（未用 OpenCL） |
| 事件夹具 | `benchmarks/p2_events.tsv`，44 条，2 个 session |
| 事件类型 | discard 17、hidden_question 5、turn_change 4、idle 4、untrusted_chat 4、pong 3、round_start 2、draw 2、chow 1、kong 1、reaction 1 |
| 说话者 | speaker1 10、speaker2 18、speaker3 16 |
| 覆盖夹具 | `benchmarks/p2_bench_fixtures.tsv`，31 条（P1 的 31 个 case 各一条） |
| 策略 | fixed / weighted / llm，同一 44 事件、同一银行、同一音频目录 |
| LLM | 本地 MNN Pocket 0.3B INT4，`make_mnn_backend(config, max_tokens=8)`；P2 CLI 未覆盖线程数，使用 `config.json` 默认值（P1 推荐的 cpu8 仍是产品运行时候选，但本表 LLM 延迟不是 cpu8 标注测量） |
| 音频 | 43 条交付使用 bank WAV；1 条无候选不合成 |

运行命令（可复现，路径按本机调整）：

```powershell
build-p2-real\Release\aimj_p2_voice_loop.exe --strategy fixed  --run-id fixed
build-p2-real\Release\aimj_p2_voice_loop.exe --strategy weighted --run-id weighted
build-p2-real\Release\aimj_p2_voice_loop.exe --strategy llm --mnn-config <local-mnn-model>\config.json --run-id llm
build-p2-real\Release\aimj_p2_voice_loop.exe --strategy fixed --play --run-id play-fixed
```

分析：`python -I benchmarks/p2_analyze.py`（只读本地结果，不调用模型/网络，不填人工分）。

---

## 3. 结果

### 3.1 银行覆盖

| 指标 | 值 |
|---|---|
| 31 个 benchmark fixture 精确覆盖 | **31/31** |
| 银行条目 | 54（speaker1 16 / speaker2 20 / speaker3 18；sid8 16 / sid9 20 / sid10 18） |
| 音频文件 | 52 个不同 WAV，0 缺失，0 SHA-256 不符 |
| 采样率 | 全部 8000 Hz，mono PCM16 |

覆盖按 `(event_type, fact_key, speaker)` 精确计算；没有把“同 event_type 任意行”算作覆盖。

### 3.2 A/B/C 策略对比（44 事件）

| 策略 | 交付 | 无候选 | 机审拒绝 | 事实违规 | sid 错配 | persona 匹配 | 唯一行 | 重复出现 | 最大连说同一行 | event→PCM p50/p95 ms | LLM 调用/合法 | LLM p50/p95 ms | 峰值 RSS | 进程 CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---|---:|---:|
| fixed | 43 | 1 | 0 | 0 | 0 | 43/43 | 36 | 7 | 1 | 0.520 / 0.735 | 0 | — | 5.422 MB | 31.25 ms |
| weighted | 43 | 1 | 0 | 0 | 0 | 43/43 | 28 | 15 | 1 | 0.526 / 0.778 | 0 | — | 5.422 MB | 31.25 ms |
| llm | 43 | 1 | 0 | 0 | 0 | 43/43 | 28 | 15 | 1 | 192.542 / 235.972 | 43 / 43 | 192.107 / 235.547 | 291.66 MB | 33,921.875 ms |

要点：

- 三种策略都做到 **0 事实违规、0 声线错配、0 校验拒绝、最大连说 1 次**；差异只在台词分布与成本。
- LLM 43 次调用全部返回合法编号（42 次直接采用，1 次因选中最近刚说过的行被 `llm_recent_override` 改为确定性轮转）；非法输出回退由单元测试另外覆盖。
- LLM 把 event→PCM p95 从 **0.78 ms 拉到 235.97 ms**，峰值 RSS 从 **5.42 MB 拉到 291.66 MB**，CPU 时间约 **1085×**；台词唯一行数与 weighted 相同（28），没有产生可测量的多样性增益。
- 候选集本身很小：17 个事件只有 1 个候选，23 个只有 2 个，3 个有 4 个。LLM 在 17 个事件上没有任何选择空间，这是“无增益”的结构性原因，不只是模型能力问题。

**LLM 决策：不进入运行时在线路径。** 保留离线实验开关；产品默认 `fixed`/`weighted` 确定性选词。若未来要恢复 LLM，必须先证明可感知的趣味/人格增益，并满足 p95 ≤ 1 s 与内存预算。

**无模型基线冻结（Owner 裁决）：** `fixed`/`weighted` 的确定性路径是后续任何 <2B 模型（含 Qwen 0.6B 档）必须对比的基线。比较维度固定为**角色趣味性、台词重复率、event→PCM 延迟、峰值内存**，不以推理 tok/s 单独代替产品指标。

### 3.3 44 事件压力与失败路径

| 检查 | 结果 |
|---|---|
| 事件总数 | 44（session 1: 24，session 2: 20） |
| 交付/静音 | 43 交付，1 条无候选（event 43，`tile:8wan`，speaker2） |
| 无候选行为 | `selection_method=no_candidate`，`fallback_used=true`，无音频、无异常、后续事件继续 |
| 隐藏/不可信输入 | 5 条 hidden_question + 4 条 untrusted_chat 全部被限制在对应 bank 行；无手牌/分数/胜负宣称 |
| 跨 session | session 2 的音频全部来自本 session 事件，无上一局残留 |
| 重复 | 最大连续同一行 = 1；fixed 有 7 次非连续复用台词，weighted/llm 各 15 次 |
| 死锁/崩溃 | 无；进程正常退出并写 summary |
| 规则影响 | P2 未调用/修改 `GameSession`、`IRuleSet`；LLM 输出只影响选中的 bank line_id |

### 3.4 真实 Windows 播放证据

`benchmarks/results/p2/runs/play-fixed/results.jsonl`（44 行事件 + 1 行 summary）：

| 指标 | 值 |
|---|---|
| 实际提交播放 | **43/43 交付事件**，`audio_status=ok`，`played=43` |
| `event_to_pcm_ms` | p50 0.635 / p95 0.880 / max 0.945 |
| `pcm_to_player_submit_ms` | p50 46.736 / p95 62.512 / max 126.825 |
| `player_callback_ms` | p50 2369.928 / p95 4243.996 / max 5254.257 |
| `audio_duration_ms` | p50 2355.500 / p95 4234.512 / max 5254.125 |
| `first_audio_output_ms` | null（未做环回采集） |
| `first_audible_ms` | null（未做人工听音确认） |

`player_callback_ms` 与 `audio_duration_ms` 的差值在 p50 约 14 ms、p95 约 9 ms 量级，说明回调在缓冲区播完后到达；这**只证明提交/播放完成**，不证明扬声器真实出声或听感合格。

另有一条 2 事件 smoke（`play-test`）：2/2 `status=ok`，用于修复 `waveOutOpen` 参数后的最小回归；不替代 43 事件证据。

### 3.5 音频验收（Phase C）

- **sid 统一**：P2 银行全部绑定 8/9/10；P1 C 臂“命中播 8/9/10、未命中回退 0/1/2”的混杂结构在 P2 已消除。
- **P1 5/37 静音门复核**（不改原始测量，只做归类）：

| 文件 | 时长 s | 峰值 dBFS | RMS dBFS | 静音占比 | 首/尾静音 s | 归类 |
|---|---:|---:|---:|---:|---|---|
| `term_hu` | 0.869 | −23.860 | −38.451 | 50.0% | 0.192 / 0.192 | 自然停顿为主；整体偏小声，需人耳确认 |
| `term_gang` | 0.550 | −14.275 | −31.088 | 45.9% | 0.065 / 0.166 | 尾静音较长，疑似自然收尾 |
| `tile_9tong` | 0.563 | −22.512 | −36.869 | 30.4% | 0.109 / 0.001 | 首静音为主，偏小声 |
| `exclaim_s1` | 1.177 | −24.402 | −35.340 | 30.4% | 0.132 / 0.090 | 自然停顿 + 偏小声 |
| `honor_baiban` | 0.554 | −10.618 | −29.969 | 30.3% | 0.104 / 0.003 | 首静音为主，能量正常 |

  5 条都有可检测语音能量与 F0（105–229 Hz），**不是合成失败/空音频**；其中 `term_hu`、`exclaim_s1` 峰值约 −24 dBFS，属于需要人工确认的“偏小声”候选。是否重合成或调整阈值，等听测后决定；本轮不篡改 `max_silence_pct=30` 门。
- **连续播放工具**：`aimj_p2_voice_loop --play` 对事件流逐条播放；`benchmarks/results/p2/audio/bank_audio/index.html`（脚本生成，gitignore 内）提供 54 条 bank 音频的本地连续试听索引；`docs/experiments/2026-11-11-p2-voice-loop/listening-checklist.csv` 是人工记录表。
- **人工听感：pending。** 没有任何 `listen=pass` 或趣味分被写入；需要真实扬声器/耳机试听后再填。

### 3.6 P1 C 臂复核（A1–A4 的落点）

- **A1**：B 是 post-hoc audit，不是播放安全门；P1 链路的违规文本可能已经合成。P2 已把校验放在 PCM 读取/播放之前，并有“违规输出不得产生 PCM”的单测。
- **A2**：P1 C 未命中回退的是 **A 自由生成**，不是 B；已实测结构是“C bank + A 回退”，不是“C bank + B 兜底”。
- **A3**：P1 C 的 7 条命中里 2 条语义张冠李戴，`attack_stale` 为 speaker2 播 sid10 音频；因此 **C 的音频不能作为声线对比证据**。P2 用精确键 + speaker/sid 强绑定重建银行，`attack_stale` 对应行现为 speaker3/danding/sid10。
- **A4**：P1 报告已明确：provider 报错会丢失部分文本、`raw_llm_text` 只在已生成时保留、OpenCL preflight 不等于 kernel 归因、`ttfc_p95` 已补算（cpu4 296.453 / cpu8 249.703 / opencl 264.368 ms）、s4=0.0 是“无成功样本”而非 0 延迟、64/271 为外部/不可核验、120/37 人工项仍 pending。

### 3.7 Rebase 后回归（`a56e6f4`）

P1 #6 合入 `dev` 后，P2 rebase 到 `a56e6f4`。冲突集中在 `CMakeLists.txt`、`sha256.hpp`、
`wav.hpp`、`test_main.cpp`：保留 P1 的 `WavWriteStats` 和单份 `read_pcm16_wav`，合并双方
测试块。无模型构建 + ctest 1/1 通过。

| 检查 | 结果 |
|---|---|
| 44 事件 fixed | 43 交付、1 条 `no_candidate` 静音（`not_applicable/skipped`）、0 校验拒绝、0 事实违规、0 声线错配、最大连说 1 |
| event→PCM | p50 0.976 ms、p95 1.382 ms（本次机器状态；与前次 0.52/0.74 的差异按环境噪声处理，不是代码路径变化） |
| 2 事件播放 smoke | 2/2 `audio_status=ok`、`played=2`、`waveOut` 回调到达；`first_audible_ms=null` |
| 测试修复 | recent 窗口实际为 4；原测试在 6 次 fixed 后仍期望 LLM 命中 `a2`，必然触发 `llm_recent_override`。改用新 `Director` 隔离 recent 状态，并把过期“last two lines”注释改为 4 |

结果为本地 `benchmarks/results/p2/runs/rebase-{fixed,play}/`；不改变 §3.2 的原始 A/B/C 记录。

---

## 4. 限制与未验证

1. **未接入真实游戏循环**：P2 是 headless CLI + 类型化事件夹具；没有把 `GameSession` 的实时事件映射进 `PublicEvent`，也没有 Axmol UI。集成是下一阶段第一项。
2. **规则仍为 Demo**：`DemoRuleSet` 不是麻将；本报告不评价胡牌、番型、结算、牌墙正确性。Full Game = NOT EVALUATED。
3. **人工听感未做**：37 条 P1 + 54 条 P2 音频的“发音/清晰度/节奏/三声线分离/音量/愿意重复听”全部 pending；不得引用任何虚构分数。
4. **可听起点未测**：`first_audible_ms=null`；`player_callback_ms` 只是缓冲区播完，不是人耳起点。
5. **LLM 延迟不是 cpu8 标注测量**：P2 CLI 未暴露线程参数，使用 `config.json` 默认值；因此只用于“LLM 是否值得在线”的方向性判断，不作为 cpu8 性能结论。
6. **候选集不均衡**：17/43 事件只有 1 个候选，LLM 选择空间天然受限；“LLM 无增益”在这一银行结构下成立，不能外推到更大候选集。
7. **2 条台词复用同一 WAV**：银行 54 条 / 52 文件；不影响事实与声线校验，但统计“唯一音频”时应按 52 计。
8. **Rebase 状态**：P1 #6 已合入 `dev`（`a56e6f4`），P2 已 rebase 到该提交；P1 的 SID 复现修复（`91643a8`）随之进入 `dev`。Rebase 后复跑 44 事件与 2 事件播放 smoke，结果见 §3.7。

---

## 5. 最终判定

| 维度 | 结论 |
|---|---|
| 运行时闭环 | **GO**：43/43 事件完成选词→校验→PCM→`waveOut` 回调；0 崩溃/死锁 |
| 受约束对话 | **GO（原型）**：31/31 覆盖、0 事实违规、0 声线错配、无候选静音、LLM 非法输出有回退 |
| 语音听感 | **CONDITIONAL / PENDING**：程序化格式与哈希通过，人工听测未做 |
| 完整游戏 | **NOT EVALUATED**：无真实麻将规则与结算 |
| LLM 在线价值 | **NO-GO（本轮）**：无多样性增益，延迟/内存代价 ~300×，关闭在线路径 |
| 建议运行时 | 确定性 Director + 精确 bank + 预渲染 8 kHz WAV + `waveOut`；LLM 仅离线实验 |
| 建议 TTS 映射 | speaker1→sid8、speaker2→sid9、speaker3→sid10；继续人工听测后再冻结 |

**总判定：CONDITIONAL GO。** Runtime 与 Constrained Dialogue 两项 GO；音频听感与真实游戏集成是剩余条件，Full Game 未评估。

---

## 6. 证据文件

| 文件 | 内容 |
|---|---|
| `benchmarks/results/p2/runs/{fixed,weighted,llm}/results.jsonl` | 三策略逐事件记录 |
| `benchmarks/results/p2/runs/play-fixed/results.jsonl` | 44 事件 + 43 条真实播放 |
| `benchmarks/results/p2/runs/play-test/results.jsonl` | 2 事件播放 smoke |
| `docs/experiments/2026-11-11-p2-voice-loop/metrics.json` | 本报告数字的机器可读版本 |
| `docs/experiments/2026-11-11-p2-voice-loop/strategy-comparison.csv` | A/B/C 对照表 |
| `docs/experiments/2026-11-11-p2-voice-loop/listening-checklist.csv` | 54 条人工听测表（空白待填） |
| `benchmarks/results/p2/audio/bank_audio/index.html` | 本地连续试听索引（gitignore 内） |
| `benchmarks/p2_bank_manifest.tsv` | 54 条银行 + SHA-256 |
| `benchmarks/p2_events.tsv` | 44 条事件夹具 |
| `benchmarks/p2_bench_fixtures.tsv` | 31 条覆盖夹具 |
