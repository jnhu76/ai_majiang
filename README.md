# AI Mahjong — Development Branch

Native/offline Windows, Android and iOS AI Mahjong in C++20.

This branch (`dev`) is the **daily integration branch**, not a release. The **verified-in-Linux headless architecture slice** is promoted here from the prototype: `GameSession`, `DemoRuleSet` (a button trigger, **not real Mahjong**), `CharacterDirector`, bounded asynchronous LLM/TTS worker interfaces, mock providers, CLI benchmark, CMake tests.

**Not included here:** unverified Axmol scene integration, unverified real MNN-Pocket-0.3B adapter, unverified sherpa-onnx adapter, model/voice weights. These remain on [prototype/p0-ai-runtime](../../tree/prototype/p0-ai-runtime).

## Build without external AI SDKs

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/aimj_bench --events 20 --hz 10 --llm-delay-ms 80 --tts-delay-ms 50
```

On Windows with a multi-config generator, the benchmark path is typically `./build/Release/aimj_bench.exe`.

The prototype's mock results do not measure MNN inference or real TTS performance. Any platform validation must include hardware, models and build SHA.

## Branch policy

- `main`: accepted stable/deliverable functionality, currently architecture/project baseline only
- `dev`: daily integration; feature/fix PRs target this branch
- `prototype/p0-ai-runtime`: isolated architecture and hardware-performance research

See [docs/BRANCHING.md](docs/BRANCHING.md), [architecture](docs/ARCHITECTURE.md), and [benchmark plan](docs/BENCHMARK_PLAN.md).
