# 交付门禁披露：Mimosa L3 对 3 条 advisory-only path-traversal finding 的硬拦截

状态：本实验的**内容交付已完成但 commit 被安全门禁拦截**。报告与证据（同目录 REPORT.md v2、metrics.json、
dialogue-review.csv、audio-review.csv、hardware.json、software-manifest.json、results-samples.jsonl）已脱敏并就绪；
脚本防护已实现并实测；被拦截的是最后一步 `git commit`。本文件记录全部细节，不改动已评审的 REPORT.md。

## 1. 三条 finding（file:line 为仓库相对路径）

| # | 位置 | 标题 | 级别 | advisory | verdictEffect | 污点链 |
|---|---|---|---|---|---|---|
| 1 | `benchmarks/p1_audio_analyze.py:225` | main 是 path-traversal 入口 | high | true | none | 命令行参数流入 main() → main(sink:path-traversal) |
| 2 | `benchmarks/p1_analyze.py:409` | main 经 1 跳到达 path-traversal | high | true | none | 命令行参数流入 main() → main → write_review_input(sink:path-traversal) |
| 3 | `benchmarks/p1_tts_qc.py:169` | main 是 path-traversal 入口 | high | true | none | 命令行参数流入 main() → main(sink:path-traversal) |

- 三条均为**污点入口提示**：不可信源是 `sys.argv`（CLI 工具按契约必须接收路径参数），sink 是分析脚本自身的文件读写。
- 扫描器自判字段：`advisory: true`、`verdictEffect: "none"`、`cwe: []`、`evidenceState: "static-finding"`，
  finding 原文注明「静态 advisory 需要人工确认真实数据流和可利用性」——即 Mimosa 扫描器自身判定其**不影响 verdict**，
  但 L3 git 门在 graded 模式下对所有 high 一律强制 deny。
- 扫描证据（Mimosa 官方工具产出）：scanId `scan-2026-11-11T03-31-22.669Z-db50bf6b3369`（seal
  `sha256:6f55b03c2ba73173db7d68f6ff2eafbb69cdc832ab4f95d7df5e8677da340a34`，deep、聚焦 3 个脚本，findingCount=3）；
  前序 scanId `scan-2026-11-11T03-29-52.916Z-a25747692684`（seal `sha256:d7e511da6577dd1722048aa86031fa3904f3661167ba9b41be44facef0fca727`，findingCount=1）。

## 2. 已实现的真实防护（与被拦截的提交同批交付）

三个分析脚本的 CLI 路径参数现在全部经 `repo_confined()` 在 **argparse `type=` 解析期**处理：

1. **规范化**：`pathlib.Path(raw).resolve()`（消解 `..` 与符号链接）。
2. **显式拒绝 `..`**：路径段级检查，含 `..` 段直接 `SystemExit`（即使解析后仍在仓库内也拒绝，如 `tmp/../tmp/x`）。
3. **仓库限定**：`resolved == repo or resolved.is_relative_to(repo)`，否则 `SystemExit`；仓库根由 `__file__` 推导，不依赖 cwd。
4. **NUL/空串检查**。

实测记录（本会话执行，accept/reject 双向）：

- `python -I benchmarks/p1_audio_analyze.py --root tmp/guard-selftest --out tmp/guard-selftest` → 正常产出 audio-analysis.json / audio-review.csv；
- `python -I benchmarks/p1_audio_analyze.py --root tmp/guard-selftest --out C:/Windows/Temp` → `--out escapes the repository`（exit 1）；
- `--out ../outside` → `--out must not contain '..' path segments`；
- `--root tmp/../tmp/x` → 同上（解析期拒绝）；
- `python -I benchmarks/p1_tts_qc.py --wav-dir tmp/guard-selftest --out tmp/guard-selftest` → exit 0。

REPORT.md §7 文档化的全部复现命令均不使用 `..` 且位于仓库内，防护不破坏任何已文档化用法。

## 3. 被拦截的命令与钩子原文

拦截发生在 2026-11-11 会话内对下述命令的三次尝试（每次按钩子建议修复后行号随编辑移动，advisory 本身不消失）：

```
git commit -m "feat(p1): wire real MNN/sherpa probe harness with audit trail and OpenCL verification"
```

PreToolUse 钩子原文（第三次，路径已写为仓库相对形式）：

> Mimosa L3 在 commit 前发现 1 个高危，最高等级 high：
> `<repo>/benchmarks/p1_audio_analyze.py:225` [high] main 是 path-traversal 入口。高危已强制拦截，请修复并重新扫描。
> 本次覆盖不完整，不能把未发现更多问题解释为项目安全。

首次拦截同时包含 `tmp/consistency-check/check7.py:127`（脚本内**真实硬编码本机绝对路径**）——该 scratch 文件已删除
（其产出 check7.out 保留，结论已并入 REPORT.md v2）；三个分析脚本的入口 advisory 属另一类（见 §1），无法在不删除
CLI、不破坏复现契约的前提下消除。

## 4. Owner 放行步骤（warn 模式：仍扫描、仍显示这 3 条，可审计；不采用完全关闭）

PowerShell（设置后**必须重启 ZCode**，PreToolUse 钩子进程才会继承环境）：

```powershell
setx MIMOSA_GIT_GATE_MODE warn
```

重启后（staged 内容已就绪，含本文件）：

```
git commit -F tmp/delivery/commit-message.txt
git push -u origin experiment/p1-dialogue-real
gh pr create --base dev --head experiment/p1-dialogue-real --title "P1 真实 AI 麻将对话与语音实验" --body-file tmp/delivery/pr-body.md
gh issue comment 5 --body-file tmp/delivery/issue-comment.md
```

`tmp/delivery/` 下另有完整补丁备份：`git diff --cached --binary` 产物 `tmp/delivery/p1-dialogue-real.patch`。

## 5. 边界说明

- 三个脚本的路径防护保留，不为过扫描器改逻辑；不做 `--no-verify` / plumbing 绕过 / 改 `.mimosa` 状态。
- 最终 merge 由 Owner 决定。
- 若后续扫描出现**非 advisory 或 verdictEffect 非 none** 的 high finding，应停止放行并先行修复。
