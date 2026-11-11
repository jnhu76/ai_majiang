# AI Mahjong Lab — Offline Native Architecture & Boundary Bench

A **real C++20 architecture slice** for a future four-player, single-player, entirely offline AI mahjong game on Windows / Android / iOS. The purpose is to test **where runtime latency, queues, memory and device constraints break** before investing in full rules or art. Axmol 2.11.5 LTS is the target presentation engine; MNN-Pocket-0.3B and sherpa-onnx are the target native providers.

**Current status:** C++20 headless core and mock-based load generator compile and pass tests on Linux. The Axmol scene is an integration overlay **not yet verified against the engine**. Optional real MNN/sherpa adapters exist, but they have **not been compiled against SDKs or run with actual model files**. Windows, Android, and iOS device performance is **unknown**. No real model/voice assets are committed.

## Architecture (same for CLI benchmark and graphical game)

```text
[Axmol TableProbeScene / CLI driver]
             |
       GameSession ---> IRuleSet (DemoRuleSet only)
             | GameEvent
       CharacterDirector
             | DialogueRequest
       AiRuntime (bounded queues)
          |          |
    1 LLM worker   1 TTS worker
    ILlmBackend    ITtsBackend
    Mock / MNN    Mock / sherpa-onnx
             | SpeechResult (PCM)
         Main-thread poll ---> subtitles / WAV playback
```

The **demo tile** is only a trigger; the project does **not yet contain legal Mahjong, scoring, actual Bot policy or final character art**. This is intentional: the interfaces and concurrency topology are the intended production architecture, whereas rules and art will be added later.

## Build and test without AI SDKs

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
mkdir -p benchmarks/results
./build/aimj_bench --events 60 --hz 10 \
  --llm-delay-ms 80 --tts-delay-ms 50 --queue 4 \
  --deadline-ms 2000 --out benchmarks/results/smoke.jsonl
```

Windows executable is typically `build/Release/aimj_bench.exe` with multi-config generators. This synthetic test runs the same `GameSession`, `CharacterDirector`, queues, providers interface and result processing as the Axmol adapter, with **mock** text and synthesized sine-wave PCM. It is *not* a claim about MNN or sherpa performance.

For a mock pressure matrix:

```bash
python benchmarks/run_matrix.py --exe build/aimj_bench --events 80 --repeats 3
```

The matrix runs 36 trials (~minutes depending on event pacing). Results are `benchmarks/results/matrix.csv` and JSONL. The generated artifacts are ignored by Git.

## Files to start with

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — ownership, dependency directions, cancellation and non-goals
- [`docs/adr/`](docs/adr/) — accepted architecture decisions, starting with [ADR-0001: NPC offline authoring and runtime behavior separation](docs/adr/0001-npc-offline-authoring-pipeline.md) (teacher LLMs are dev-time tools; runtime ships only reviewed declarative packs; see [`docs/npc/`](docs/npc/) contracts)
- [`docs/BENCHMARK_PLAN.md`](docs/BENCHMARK_PLAN.md) — real model/OS measurement matrix, performance limits, evidence protocol
- [`apps/axmol/README.md`](apps/axmol/README.md) — link the exact same core into a generated Axmol native app, show clickable demo tile / three roles / PCM playback
- [`src/mnn_backend.cpp`](src/mnn_backend.cpp), [`src/sherpa_backend.cpp`](src/sherpa_backend.cpp) — optional native providers with SDK pins to verify
- [`tests/test_main.cpp`](tests/test_main.cpp) — stale command, session invalidation and bounded queue tests

## Deliverables by milestone

1. **P0: host core + mock scheduler** (implemented and tested on Linux).
2. **P1: Axmol 2.11.5 display** (scene sources supplied, Windows build/device smoke pending).
3. **P2: MNN-Pocket-0.3B and sherpa real SDK integration** (adapters supplied, pin/build/run pending).
4. **P3: Android/iOS real device integration and boundary matrix** (pending; iOS requires Xcode on macOS).
5. **P4: replace DemoRuleSet with deterministic Mahjong engine and legal-action Bot** (pending).

## External official sources

- Axmol releases: https://github.com/axmolengine/axmol/releases
- MNN LLM build + benchmark documentation: https://github.com/alibaba/MNN/blob/master/docs/transformers/llm.md
- sherpa-onnx native C TTS API: https://k2-fsa.github.io/sherpa/onnx/c-api/html/tts.html

## License

Project code: MIT (see `LICENSE`). Third-party model weights, voice models and art have separate licenses and must be independently evaluated before redistribution.
