# Axmol display adapter — integration overlay (not CI-compiled yet)

These two files show a real Axmol scene using **the same** `aimj_core` `GameSession`, `CharacterDirector`, bounded `AiRuntime`, `AudioBuffer` and WAV bridge as the independent benchmark. The visible UI contains a green table, a tile-like button, three alternating personas, subtitle and generated PCM playback.

## Build on a Windows/macOS developer machine

1. Clone Axmol `v2.11.5` (not default `dev`) and use its CLI to generate a C++ application from the official template.
2. Add `TableProbeScene.{h,cpp}` to the generated application's `Classes/`; include the repository's `include/` directory in the app target.
3. Add the repository root as a CMake subdirectory to the generated project (with a separate binary directory if external):
   ```cmake
   set(AIMJ_BUILD_CLI OFF CACHE BOOL "" FORCE)
   set(AIMJ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
   add_subdirectory("/absolute/path/to/ai-mahjong-lab" "${CMAKE_CURRENT_BINARY_DIR}/aimj")
   target_link_libraries(${APP_NAME} PRIVATE aimj_core)
   target_sources(${APP_NAME} PRIVATE Classes/TableProbeScene.cpp)
   ```
   `APP_NAME` must be replaced with the actual Axmol template's CMake target variable/name; inspect the generated CMakeLists rather than guessing.
4. In the template's AppDelegate initial scene creation, replace the example scene with `TableProbeScene::create()` and include `TableProbeScene.h`.
5. Run the template on Windows; then build for Android and iOS with respective SDKs. These platform builds **have not been validated in this environment**.

## Native AI adapters

Initially the scene uses Mock providers. After locking the MNN runtime commit + model SHA and sherpa-onnx build + voice model, swap both `std::make_unique<Mock...>` calls with `make_mnn_backend(config_path)` / `make_sherpa_vits_backend(model,tokens,lexicon,threads)` and compile `aimj_core` using `AIMJ_WITH_MNN` / `AIMJ_WITH_SHERPA` with pinned SDK libraries. Properly resolve writable paths for assets on mobile. The native SDK builds, platform linker requirements, and per-platform audio behavior still require verification.

### Known limitations / remaining engineering

- The `audio/AudioEngine.h` include and app target linkage should be confirmed against your specific 2.11.5 template build.
- WAV disk bridge means speech latency measured by `aimj_bench` stops at generated PCM, before disk write and audio hardware playback. Add trace points for disk write, audio play and first audible sample.
- Device lifecycle and background/pause handling are not implemented.
- The tile is an **event trigger**, not a complete mahjong game or full tileset.
- Real GPU/CPU memory, frame-time, thermal, energy and audio-device latency cannot be measured until on-device integration.
