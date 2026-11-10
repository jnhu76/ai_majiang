# Stage 3: aimj_bench with both REAL providers (MNN-Pocket LLM -> sherpa VITS PCM).
$extern = "$PWD\..\ai_majiang-extern"  # SDK root outside the repo (see 00-setup-extern.ps1)
$env:HTTP_PROXY  = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
$sha = "24563667d4c923a6f67591a0132141a84b1e2ebb"
if (-not (Test-Path "$extern/third_party/MNN")) {
  git init $extern/third_party/MNN; Push-Location $extern/third_party/MNN
  git remote add origin https://github.com/alibaba/MNN.git
  git fetch --depth 1 origin $sha; git checkout -q FETCH_HEAD; Pop-Location
}
# 1) MNN CPU runtime (also builds MNN.dll)
cmake -S $extern/third_party/MNN -B build-mnn -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DMNN_BUILD_LLM=ON -DMNN_LLM_BUILD_DEMO=ON -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF `
  -DMNN_BUILD_TEST=OFF -DMNN_BUILD_BENCHMARK=OFF -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_OPENCV=OFF `
  -DMNN_BUILD_AUDIO=OFF -DMNN_IMGCODECS=OFF -DMNN_BUILD_DIFFUSION=OFF
cmake --build build-mnn --config Release
# 2) aimj with both real adapters
$repo = $PWD.Path
cmake -S . -B build-real -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DAIMJ_WITH_MNN=ON `
  -DAIMJ_MNN_INCLUDE_DIR="$repo/$extern/third_party/MNN/transformers/llm/engine/include;$repo/$extern/third_party/MNN/include" `
  -DAIMJ_MNN_LIBRARIES="$repo/build-mnn/Release/MNN.lib" `
  -DAIMJ_WITH_SHERPA=ON `
  -DAIMJ_SHERPA_INCLUDE_DIR="$repo/$extern/third_party/sherpa-onnx-extract/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release/include" `
  -DAIMJ_SHERPA_LIBRARIES="$repo/$extern/third_party/sherpa-onnx-extract/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release/lib/sherpa-onnx-c-api.lib"
cmake --build build-real --config Release
ctest --test-dir build-real -C Release
# 3) runtime DLLs next to the exe
Copy-Item build-mnn/Release/MNN.dll build-real/Release/ -Force
$s = "$extern/third_party/sherpa-onnx-extract/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release"
Copy-Item "$s/lib/*.dll" build-real/Release/ -Force
Copy-Item "$s/bin/onnxruntime*.dll" build-real/Release/ -Force
# 4) 20 real events, 2 Hz, 3 s deadline
mkdir -Force benchmarks/results/real-win | Out-Null
./build-real/Release/aimj_bench.exe --llm mnn --tts sherpa `
  --mnn-config "$extern/third_party/models/MNN-Pocket-0.3B-MNN/config.json" --mnn-max-tokens 48 `
  --tts-model "$extern/third_party/models/vits-icefall-zh-aishell3/model.onnx" `
  --tts-tokens "$extern/third_party/models/vits-icefall-zh-aishell3/tokens.txt" `
  --tts-lexicon "$extern/third_party/models/vits-icefall-zh-aishell3/lexicon.txt" `
  --tts-rule-fars "$extern/third_party/models/vits-icefall-zh-aishell3/rule.far" `
  --tts-threads 2 --events 20 --hz 2 --deadline-ms 3000 --queue 4 --settle-ms 60000 `
  --out "benchmarks/results/real-win/real20.jsonl"
