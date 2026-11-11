# P1 review closure — CodeRabbit 17 findings 与 GPT-6 A1–A4 的逐项回归证据

- 日期：2026-11-11
- 对应分支：`experiment/p1-dialogue-real`
- 对应提交：`0effeec` + 本地 staged 修复（尚未 commit/push）
- 结论：17 条 CodeRabbit finding 全部有代码或文档落点；其中 retry 悬空指针、WAV chunk 边界、
  证据脱敏、分析脚本完整性四项已做定向回归。**CI 绿色不作为合并依据；PR #6 仍保持 OPEN。**
- 机器可读摘要：`review-regression.json`（同目录）。

---

## 1. Retry 悬空指针（CodeRabbit Major，`dialogue_probe.cpp:1036`）

### 问题

旧代码在 template retry 中：

```cpp
Work retry = w;              // 局部对象
submit(retry);               // by_event[eid] = &retry
drain(await());              // await 超时返回 nullopt 时，retry 仍在队列
```

`await()` 超时后 `retry` 离开作用域，`by_event` 保留悬空指针；迟到的 provider 结果再被 `drain`
解引用即产生未定义行为（此前实测出现过 `Po(w) r846815596` 一类损坏输出）。

### 修复

`apps/cli/dialogue_probe.cpp`：

1. `std::map<EventId, Work*> by_event` → `std::map<EventId, Work> by_event`，map 自己拥有 Work。
2. `drain()` 先移动再删除：
   ```cpp
   Work w = std::move(it->second);
   by_event.erase(it);
   ```
3. `submit()` 返回 `std::optional<EventId>`；队列拒绝时立即 `erase`，调用方只对成功的 id 等待。
4. `await()` → `await_event(EventId want)`：只接受 `result->event_id == want`；其它事件的结果在
   自己的 Work 名下 `drain`，不会误记到当前 case。
5. retry 走 `if (auto retry_id = submit(retry)) drain(await_event(*retry_id));`，retry 局部对象
   在 `submit` 返回前已被复制进 map。

### 回归证据

- 静态：`rg -n 'Work\*|&retry|by_event\[' apps/cli/dialogue_probe.cpp`，已无 `Work*` / `&retry`。
- 运行：124 个事件（4 persona × 31 case）、`MockLlm(10ms)`、`--deadline-ms 1`，让每个结果都在
  deadline 之后到达：

  ```powershell
  build-core-ci\Release\aimj_dialogue_probe.exe --battery dialogue --rounds 1 `
    --llm mock --tts mock --deadline-ms 1 --queue 2 `
    --output tmp/verify-late --run-id late --force
  ```

  结果：`124 timeout`、`unresolved=0`、`raw_llm_text` 非空 124、`fallback_used` 124、无损坏
  event id、无崩溃。
- 残余限制：当前 mock 的延迟是固定的 10 ms，无法构造“第一次成功且违规、retry 特别慢”的
  定向 retry 超时回放；因此 retry 路径按**所有权结构**关闭，迟到结果归因由上述 124 事件运行覆盖。
  若要把 retry 超时也做成可重复运行，需要给 probe 增加一个 per-call 延迟注入后端；本轮未加。

---

## 2. WAV chunk 边界（CodeRabbit Minor，`wav.cpp:68`）

### 修复

`src/wav.cpp`：

- `fmt ` chunk `size > 1024` 直接 `runtime_error`，不再按文件声称的 4 GiB 分配。
- `size` 为奇数时跳过 RIFF pad byte，避免把 pad 当下一 chunk 的头部。
- `data` chunk 原有 `1<<28` 上限保留。

### 回归证据

`tests/test_main.cpp` 新增：

- `aimj_test_huge_fmt.wav`：`fmt ` size = `0xFFFFFFF0`，期望 `runtime_error`（不是 `bad_alloc`）。
- `aimj_test_padded_fmt.wav`：`fmt ` size = 17 + 1 pad，后面接 `data`，期望正常读到
  `sample_rate=8000` 和 1 个样本。

运行：

```powershell
ctest --test-dir build-core-ci -C Release --output-on-failure
# 100% tests passed, 0 tests failed out of 1
```

---

## 3. 证据脱敏（CodeRabbit Minor，`04-run-p1-matrix.ps1:40`）

### 修复

`docs/experiments/2026-11-11-p1-dialogue-real/repro/04-run-p1-matrix.ps1`：

- `Convert-ToRepoRelative`：把 repo/extern 根路径替换为 `<repo>/...`、`<extern>/...`，其它路径
  只保留文件名并标 `<external>/`。
- `Convert-EvidenceText`：对 `results.jsonl` / `console.out` / `console.err.out` 同时替换
  原始、JSON 转义、正斜杠三种形式的绝对路径。
- `provenance.json` 的 probe/MNN DLL/model/VITS 路径全部先过 `Convert-ToRepoRelative`。
- `runner.out` 移到 gitignore 的 `benchmarks/results/.../runs/$RunId/`，不再写入 committed evidence。

### 回归证据

```powershell
git grep -nE '[A-Za-z]:[\\/](Users|Documents)|/home/[a-z]' -- .
# 无输出
git diff --cached | rg '[A-Za-z]:[\\/](Users|Documents)|/home/[a-z]'
# 无输出
```

`software-manifest.json` 只含相对路径（`build-mnn/Release/MNN.dll`、
`build-mnn-opencl/Release/MNN.dll`）与 SHA-256，不含本机绝对路径。

---

## 4. 分析脚本数据完整性（CodeRabbit 4 条 + GPT-6 A4）

| 问题 | 修复 | 定向回归 |
|---|---|---|
| `merge_reviews` 先截断再验 header | 先验证全部 `group-*.csv` header，再写 `.tmp`，最后 `replace` | 坏 header 时 exit 1，原 `dialogue-review.csv` SHA-256 不变 |
| `merge_reviews` 把人工确认重置为 pending | 以 `(case_id,strategy,persona)` 读回旧值，只对缺失行写 pending | 旧值 `confirmed` 在 re-merge 后保留 |
| 坏 WAV 让整批分析中止 | `read_wav` 拒绝 `channels<=0`；`analyze_root` 逐文件捕获 `ValueError/OSError`，写 `listen=error: ...` | `corrupt.wav` 生成 error 行，`summary.errors=1`，好文件仍分析 |
| `ttfc_p95_ms` 门没有计算值 | `merge_configs` 增加 `ttfc_p95_ms` | metrics：cpu4 296.453、cpu8 249.703、opencl 264.368 ms |
| `--lines` 缺失时静默空映射 | 缺失即 `SystemExit`；默认路径改为 `HERE / p1_tts_lines.tsv` | 缺失文件 exit 1，输出 `lines file not found: ...` |

其余审查项：session flip 改到 work-item 边界、`cpu_fallback` 独立标签、stale/timeout 只在
`generated` 时保留 `raw_llm_text`、late-LLM 测试放宽时序、`01b` 改用 committed manifest 并
硬失败、`04` 重载 manifest、TTS 电池 sid 改为 8/9/10、REPORT 删除不存在的 `--out` 参数。

---

## 5. 合并条件核对

| 条件 | 状态 |
|---|---|
| CI 绿 | 是（旧 HEAD `0effeec`，3/3 core + CodeRabbit check） |
| 高危 review 已解决/记录 | 代码已修复；尚未 push，GitHub 线程未回复 |
| 报告与代码一致 | 本地 v3 已一致；远端 HEAD 仍是 v2 |
| 未新增规则引擎/产品功能 | 是 |
| 未改 `main` | 是 |
| 未绕过门禁 | 是；`MIMOSA_GIT_GATE_MODE` 未在 ZCode 进程内改动 |

**结论：不满足合并条件。** 需要 Owner 先放行 Mimosa、提交并推送这 15 个文件，更新 PR #6 正文
与 review 线程，等新 HEAD CI 通过后再合入 `dev`。
