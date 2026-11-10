# Local smoke and stress evidence — 2026-11-10

**This is a scheduling-only MOCK experiment. No MNN-Pocket-0.3B model, sherpa-onnx TTS or Axmol renderer was used. These results cannot be interpreted as real device or model performance.**

Environment: Linux 6.18.57 x86-64 (container), GCC 14.2.0, CMake 3.31.6, Release build. CTest 1/1 passed, plus a Debug AddressSanitizer/UndefinedBehaviorSanitizer test run 1/1 passed.

| Scenario | Offered | Accepted | Queue rejected | Completed | OK | Deadline expired | OK E2E p95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Low pressure (`20`, `10Hz`, `LLM 30ms`, `TTS 20ms`, queue 4, deadline 2000ms) | 20 | 20 | 0 | 20 | 20 | 0 | 50.50ms |
| Bursty (`25`, `60Hz`, `LLM 100ms`, `TTS 80ms`, queue 3, deadline 400ms) | 25 | 7 | 18 | 7 | 3 | 4 | 347.17ms |

These were measured with explicitly injected delays. The high arrival rate exceeds one LLM worker's simulated capacity; rejection and deadline expiry are expected outcomes, not proof of actual MNN limits. The `OK E2E p95` column excludes expired requests.

## Next required evidence

1. `llm_bench` with actual MNN Pocket model, plus raw logs, token metrics and model SHA.
2. Native sherpa VITS latency and generated audio RTF on each device.
3. Exact same end-to-end C++ pipeline with both REAL providers in a compiled Axmol application.
4. Frame-time, memory, thermals, app background/resume and 30-minute runs on Windows/Android/iOS.
