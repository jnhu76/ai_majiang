# Evidence-first performance and boundary plan

## What this repository currently measures

The `aimj_bench` C++ executable uses the same state machine, dialogue director and 2-worker runtime as the game's Axmol adapter. It emits one JSONL record per completed event, plus run metadata and summary. It records queue wait, LLM-call wall time, TTS-call wall time, end-to-end to PCM readiness, deadline status, rejected tasks and process peak resident memory. `MockLlm` and `MockTts` are synthetic, so **their numbers measure pipeline scheduling only**.

## What it does not measure yet

True model time to first token, prefill/decode throughput, tokenization, per-layer compute, speech real-time factor, time to first audio sample, audio playback latency, device frame-time/energy/temperature, OS pressure, GPU memory or thermal throttling. The real providers report total call time, not the above breakdowns. These require additional instrumentation in SDK callbacks/profilers and Axmol rendering/audio.

## Benchmark stages

| Stage | Independent variable | Observables | Decision |
|---|---|---|---|
| S0 synthetic | events/s = 2/5/10/20, mock LLM = 50/200/500ms, queue cap | queue p95, reject %, expiry %, E2E | Worker scheduler survives bursts |
| S1 MNN standalone | prompt = 32/64/128/256 tokens, generate = 16/32/64, thread counts, CPU/GPU | TTFT, prefill & decode tok/s, RSS, warmup | Find usable per-device configuration |
| S2 sherpa standalone | Chinese chars 8/16/32/64, 3 speaker IDs | TTS call latency, RTF, speaker clarity, RSS | Select voice/model |
| S3 combined real | S1+S2 concurrently on one native process | end-to-end p50/p95, rejection/expiry, RSS, CPU/GPU contention | Approve real local AI experience |
| S4 rendering | no AI vs LLM vs TTS vs both; 30+ min steady state | frame-time p95/p99, hitch count, thermal/battery drift | Stable animation while generating |
| S5 memory boundary | cold start, background/resume, peak resident, low-end RAM | OOM, load time, memory plateau, resource reload | Minimum viable device |

## Measurement protocol

- 3 warmup rounds plus 5 independent measured runs where practical; randomized run order for CPU vs GPU comparisons.
- Cold and warm loads analyzed separately. Use one fixed model hash and a fixed seed/prompt corpus for repeatability.
- Report device name/SoC/RAM, OS, thermal state, ambient conditions when available, power mode, build SHA, runtime SHA, backend, threads, model size and quantization.
- Limit output to short dialogue, enforce max tokens, text/post-processing constraints, and check speaker/style quality separately from speed.
- Use timestamps from a monotonic clock; never compare wall-clock timestamps across device/process boundaries without clock calibration.
- For mobile apps compare Foreground 60FPS target with total PSS/RSS and system memory warning events; Linux `ru_maxrss` or Windows peak working set is only a partial memory view.

## Thresholds for experimentation (not promises)

- End-to-end PCM readiness p95: ≤ 2000ms for a 20–30 Chinese-character dialogue, excluding playback.
- Deadline expiration + queue rejection combined ≤ 5% under a realistic event arrival profile; stress sweeps may intentionally exceed.
- No rule input/action blocked by LLM/TTS; 60FPS target frame-time ~16.7ms and no sustained p99 frame spikes from model concurrency.
- Stable 30-minute foreground sessions and background/resume smoke tests without memory growth or device thermal collapse.

## Real provider instructions

First run MNN's **official** `llm_bench` on the target system (not the game) using `-pg 64,32` etc. MNN documentation recommends `-pg` to capture decode under a realistic KV cache, rather than optimistic decode-only `-n` numbers. Record raw output. For mobile, build MNN with LLM enabled and CPU/Metal/OpenCL according to platform and model compatibility.

Then compile the optional `AIMJ_WITH_MNN` adapter, `AIMJ_WITH_SHERPA` adapter, and run the *same* `aimj_bench` with `--llm mnn --tts sherpa`, using SDK-specific include/library paths. Example:

```bash
cmake -S . -B build-real -DAIMJ_WITH_MNN=ON -DAIMJ_WITH_SHERPA=ON \
  -DAIMJ_MNN_INCLUDE_DIR=/path/to/MNN/transformers/llm/engine/include \
  -DAIMJ_MNN_LIBRARIES='/path/to/libllm.so;/path/to/libMNN.so' \
  -DAIMJ_SHERPA_INCLUDE_DIR=/path/to/sherpa-root \
  -DAIMJ_SHERPA_LIBRARIES='/path/to/libsherpa-onnx-c-api.so'
cmake --build build-real --config Release
mkdir -p benchmarks/results
./build-real/aimj_bench --llm mnn --tts sherpa \
  --mnn-config /path/to/pocket/config.json \
  --tts-model /path/to/model.onnx --tts-tokens /path/to/tokens.txt \
  --tts-lexicon /path/to/lexicon.txt --tts-threads 2 \
  --events 50 --hz 2 --out benchmarks/results/real.jsonl
```

The exact library names, transitive dependencies and API compatibility depend on the **pinned versions**. This example is an integration starting point, **not** a verified build recipe for all three platforms. The required runtime/model assets must be bundled offline.

## Replicability and evidence integrity

Every report must include the raw JSONL and provenance manifest, distinguish simulated vs real, and classify outcomes `PASS`, `FAIL`, `INCONCLUSIVE`. No manually fabricated measured numbers. Capture vendor benchmark output, render trace and OS memory/thermal trace in addition to game JSONL.
