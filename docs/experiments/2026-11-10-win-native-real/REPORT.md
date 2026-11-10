# P0 Windows Native Real AI Bring-up — 2026-11-10

**All numbers below marked REAL come from the pinned official model binaries on this machine.** Earlier `amd-win-mock.jsonl` scheduling numbers remain MOCK and are not restated here as model performance. Classification: stages 1–3 **PASS (REAL)**, stage 4 GPU **PASS (feasible; decode not beneficial)**.

## A. Environment (REAL)

- AMD Ryzen 7 5800H (8C/16T), 28.9 GB RAM, Windows 11 Pro x64 build 26200
- MSVC v143 14.44.35207, Windows SDK 10.0.26100.0, CMake 4.3.2, VS 2022 generator `-A x64 -T v143,host=x64`, Release, C++20
- MNN runtime: `alibaba/MNN` @ `24563667d4c923a6f67591a0132141a84b1e2ebb` (shallow clone, zero local edits). On WIN32 `MNN_SEP_BUILD` is forced OFF and the LLM engine is linked into `MNN.dll`; consumers link only the `MNN.lib` import library.
- LLM: `taobao-mnn/MNN-Pocket-0.3B-MNN` HF rev `05e05fd35206c6b1f9c662e7c8e56af21cd6b836` (qwen3 arch, INT4, tie-embeddings, mtok tokenizer). All file SHA-256 in [model-sha256.txt](model-sha256.txt) match the HF LFS oids.
- TTS: sherpa-onnx `v2.0.1` official win-x64 shared /MD prebuilt + `vits-icefall-zh-aishell3` from the official `tts-models` release tag. AISHELL-3 corpus is Apache-2.0 (218 anonymous speakers, no celebrity voices); icefall and sherpa-onnx are Apache-2.0.

## B. LLM (REAL — PASS)

Real Chinese output from `llm_demo` (prompt "用一句话介绍麻将的起源。", config `thread_num: 4`):

> 麻将的起源可追溯至中国古代，最初是作为一种民间游戏流传，后逐渐发展为一种需要特定规则和技巧的竞技方式，如今已成为中国传统的娱乐形式。
> prompt tokens = 16, decode tokens = 39, prefill 363.46 tok/s, decode 121.40 tok/s

Official `llm_bench -pg 64,32 -rep 3` (CPU INT4, ± = std over 3 reps):

| threads | prefill pp512 | decode tg128 | pp64+tg32 |
|---|---|---|---|
| 8 | 557–580 tok/s | 101–111 tok/s | 523–666 / 90–116 tok/s |
| 4 | 372–501 | 120 | 393–501 / 98–114 tok/s |
| 2 | 283–328 | 93 | 310–327 / 89–93 tok/s |
| 1 | 173–201 | 66 | 186–202 / 59–67 tok/s |

Full matrix (threads 1/2/4/8 × pg 32,16 / 64,32 / 128,32 / 256,32 / 64,64, 3 reps, JSON output): kept locally at `benchmarks/results/mnn-bench/llm_bench_matrix.{json,log}`.

- Cold model load: **0.72 ± 0.02 s** (163.54 MiB weights, `llm_bench -load true`)
- Peak memory of `llm_demo` (100 ms sampling): **≈250 MB working set / ≈286 MB private bytes**
- Measured TTFT (first emitted character, `aimj_bench` instrumented): 97–147 ms median 117 ms at ~56-token templated prompt

## C. TTS (REAL — PASS)

`sherpa-onnx-offline-tts` (v2.0.1 prebuilt, `--num-threads=2`, `--tts-rule-fars=rule.far`), output mono 16-bit PCM WAV at **8 kHz**:

| sid | line | text | synth s | audio s | RTF |
|---|---|---|---|---|---|
| 0 | 1 (short) | 碰！ | 0.079 | 0.571 | 0.138 |
| 0 | 2 (normal) | 别磨蹭，快出牌！ | 0.140 | 2.051 | 0.068 |
| 0 | 3 (long) | 你这牌打得肥嘟嘟的，我都等了三轮了… | 0.253 | 6.120 | 0.041 |
| 1 | 1–3 | same three lines | 0.067–0.165 | 0.417–3.717 | 0.044–0.161 |
| 100 | 1–3 | same three lines | 0.067–0.221 | 0.446–3.720 | 0.059–0.150 |

All 9 WAVs are non-empty (`wave` module verified: 1 ch, 8000 Hz, 16-bit). Speaker IDs 0/1/100 of 174 verified distinct audio. **This model exposes no streaming API; RTF here is full-synthesis time ÷ audio duration — it is not a first-audio-chunk latency.** Samples kept locally in `benchmarks/results/tts/`.

## D. Integration (REAL — PASS)

`aimj_bench --llm mnn --tts sherpa` on one native process, 20 events @ 2 Hz, deadline 3 s, queue 4, `--mnn-max-tokens 48`:

- `mode="real"`, every event record carries `mock:false`, and non-zero `llm_prompt_tokens/llm_gen_tokens` (mock runs emit `mock:true` + zero usage — the two result classes cannot be confused)
- **20/20 offered=accepted=completed=ok**, 0 rejected / expired / stale / failed
- E2E (LLM→PCM) min/median/max **186/284/344 ms** (target ≤2000 ms)
- Real measured per event: `llm_ttft_ms` 97–147, `llm_prefill_ms` ~103, `llm_decode_ms` 77–101, `tts_ms` 74–140, `tts_rtf` 0.035–0.083
- Peak process RSS 558 MB; provider init 4.4 s (both models)
- Instrumentation added (not fabricated): `ILlmBackend::last_usage()` → `prompt_tokens/generated_tokens/prefill_ms/decode_ms/ttft_ms`; MNN values come from `Llm::getContext()`, TTFT from a first-character stream timestamp; mock backends keep zeros. New regression test: a throwing LLM backend degrades to `status=error` + fallback text, no audio, and the runtime keeps serving later events (CTest 1/1 on both `build-real` and mock `build-win-x64`).
- Design guards verified: MNN output capped by `max_new_tokens`; empty generation throws; model chat template applied by MNN `response()`; TTS sid = speaker−1 against `speakers.txt`; LLM/TTS run on dedicated runtime workers (game thread only submits/polls); bounded queues, deadline + stale checks drop late work without playback; no silent mock fallback — provider selection is explicit CLI and unavailable providers exit with an error. LLM thread count is configured via the model's `config.json` `thread_num` (standard MNN mechanism); TTS threads via `--tts-threads`. GameSession remains the sole rules authority; backends only produce dialogue text/audio.

## E. GPU / AMD iGPU (REAL — PASS, decode not beneficial)

AMD Radeon(TM) Graphics driver 31.0.21923.11000 exposes OpenCL; `OpenCL.dll` present. MNN built with `-DMNN_OPENCL=ON` runs the **same pinned Pocket weights** with `-a opencl`:

| backend | pp512 | tg128 | pp64+tg32 |
|---|---|---|---|
| CPU 8T | 580 tok/s | 101 tok/s | 576/100 tok/s |
| CPU 4T | 372 tok/s | 120 tok/s | 100 tok/s decode |
| **OpenCL 4T** | **759–924 tok/s** | 64–82 tok/s | 544/78 tok/s |

No-CPU-fallback evidence: `llm_bench` process GPU dedicated-memory peak **257 MB**, shared 245 MB, **GPU engine utilization peak 98.8%** (per-PID counters), prefill 1.6× over best CPU — a CPU-only run cannot exceed the CPU-only ceiling. OpenCL kernel cache (`mnn_cachefile.bin`, ~3.7 MB) is generated on first run; the `Can't open file:tmp/mnn_cachefile.bin` line is the first-run cache miss, harmless.

**Decision: keep CPU as the integrated default.** iGPU prefill gains ~60% but decode loses 20–35% (GEMV-bound on shared-memory bandwidth), so real dialogue latency does not improve. Raw logs: `benchmarks/results/mnn-bench/llm_bench_opencl.json`, `opencl_gpumem_run.log`.

## F. Deliverables

- Branch `p0/win-native-real-ai` (from `dev`), integration commit + PR → `dev`
- Code: usage instrumentation (`runtime.hpp`, `runtime.cpp`, `mnn_backend.cpp`), `rule_fars` + token cap (`sherpa_backend.cpp`, `benchmark.cpp`), regression test (`tests/test_main.cpp`), `.gitignore` additions, this report + manifests + repro scripts
- Reproduce (run from repo root, in order): `docs/experiments/2026-11-10-win-native-real/repro/00-setup-extern.ps1`, `01-build-mnn.ps1`, `02-run-tts.ps1`, `03-run-integration.ps1`. SDK clones, model weights and prebuilt packages live in an **external root** `..\ai_majiang-extern\third_party` (outside the repository working tree — third-party sources are untrusted downloaded data and are not committed); hashes and provenance in [model-sha256.txt](model-sha256.txt).
- Local raw artifacts (not committed): `benchmarks/results/{mnn-bench,tts,real-win}`, `p0-hardware.json`

## G. Remaining blockers / limitations

- sherpa-onnx linked as the official prebuilt shared /MD package; a static or MT-runtime build would need a source build (not needed for the P0 evidence).
- LLM decode is sampled wall speed; per-token streaming timestamps inside MNN are engine-internal (TTFT measured at output boundary instead).
- iGPU decode regression is a bandwidth limitation of this iGPU class; revisit only if a future hybrid prefill-GPU/decode-CPU scheduler is desired (out of P0 scope).
- TTS output is 8 kHz (model-intrinsic); game audio pipeline must resample if it expects 16 kHz+.
