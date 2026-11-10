# Stage 3: aimj_bench with both REAL providers (MNN-Pocket LLM -> sherpa VITS PCM).
# Works from any working directory; paths are derived from this script's location.
# Repository Root ($repo) and External SDK Root ($extern) are kept separate and
# validated before configure. HTTP(S) proxy comes from ambient environment
# variables; none is hardcoded here.
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$tp     = Join-Path $extern 'third_party'
$mnn    = Join-Path $tp 'MNN'
$mnnModel = Join-Path $tp 'models\MNN-Pocket-0.3B-MNN'
$sherpaRoot = Join-Path $tp 'sherpa-onnx-extract\sherpa-onnx-v2.0.1-win-x64-shared-MD-Release'
$vitsModel  = Join-Path $tp 'models\vits-icefall-zh-aishell3'
$buildMnn = Join-Path $repo 'build-mnn'
$buildReal = Join-Path $repo 'build-real'
Write-Output "repo=$repo"
Write-Output "extern=$extern"

# 1) MNN CPU runtime at the pinned commit (also builds MNN.dll / MNN.lib)
$mnnSha = '24563667d4c923a6f67591a0132141a84b1e2ebb'
if (-not (Test-Path $mnn)) { throw "MNN source not found at $mnn - run 00-setup-extern.ps1 first" }
$head = git -C $mnn rev-parse HEAD
if ("$head" -ne $mnnSha) { throw "MNN checkout mismatch: expected $mnnSha, found $head" }
cmake -S $mnn -B $buildMnn -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DMNN_BUILD_LLM=ON -DMNN_LLM_BUILD_DEMO=ON -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF `
  -DMNN_BUILD_TEST=OFF -DMNN_BUILD_BENCHMARK=OFF -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_OPENCV=OFF `
  -DMNN_BUILD_AUDIO=OFF -DMNN_IMGCODECS=OFF -DMNN_BUILD_DIFFUSION=OFF
if ($LASTEXITCODE -ne 0) { throw "MNN cmake configure failed" }
cmake --build $buildMnn --config Release
if ($LASTEXITCODE -ne 0) { throw "MNN build failed" }

# 2) aimj with both real adapters - validate every include/lib path BEFORE configure.
$mnnEngineInc = Join-Path $mnn 'transformers\llm\engine\include'
$mnnCoreInc   = Join-Path $mnn 'include'
$mnnLib       = Join-Path $buildMnn 'Release\MNN.lib'
$sherpaInc    = Join-Path $sherpaRoot 'include'
$sherpaLib    = Join-Path $sherpaRoot 'lib\sherpa-onnx-c-api.lib'
foreach ($p in (Join-Path $mnnEngineInc 'llm\llm.hpp'), (Join-Path $mnnCoreInc 'MNN'), $mnnLib,
               (Join-Path $sherpaInc 'sherpa-onnx\c-api\c-api.h'), $sherpaLib,
               (Join-Path $mnnModel 'config.json'),
               (Join-Path $vitsModel 'model.onnx'), (Join-Path $vitsModel 'rule.far')) {
    if (-not (Test-Path $p)) { throw "missing SDK/model path: $p - run 00-setup-extern.ps1 / build MNN first" }
}
cmake -S $repo -B $buildReal -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DAIMJ_WITH_MNN=ON `
  -DAIMJ_MNN_INCLUDE_DIR="$mnnEngineInc;$mnnCoreInc" `
  -DAIMJ_MNN_LIBRARIES="$mnnLib" `
  -DAIMJ_WITH_SHERPA=ON `
  -DAIMJ_SHERPA_INCLUDE_DIR="$sherpaInc" `
  -DAIMJ_SHERPA_LIBRARIES="$sherpaLib"
if ($LASTEXITCODE -ne 0) { throw "aimj cmake configure failed" }
cmake --build $buildReal --config Release
if ($LASTEXITCODE -ne 0) { throw "aimj build failed" }
ctest --test-dir $buildReal -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "CTest failed" }

# 3) Runtime DLL search path: required DLLs must sit next to the exe.
$release = Join-Path $buildReal 'Release'
Copy-Item (Join-Path $buildMnn 'Release\MNN.dll') $release -Force
Copy-Item (Join-Path $sherpaRoot 'lib\*.dll') $release -Force
Copy-Item (Join-Path $sherpaRoot 'bin\onnxruntime*.dll') $release -Force
foreach ($dll in 'MNN.dll', 'sherpa-onnx-c-api.dll', 'sherpa-onnx-cxx-api.dll',
                'onnxruntime.dll', 'onnxruntime_providers_shared.dll') {
    if (-not (Test-Path (Join-Path $release $dll))) { throw "runtime DLL missing after copy: $dll" }
}

# 4) 20 real events, 2 Hz, 3 s deadline (aimj_bench exits non-zero if any event is missing).
$resultDir = Join-Path $repo 'benchmarks\results\real-win'
New-Item -ItemType Directory -Force $resultDir | Out-Null
& (Join-Path $release 'aimj_bench.exe') --llm mnn --tts sherpa `
  --mnn-config (Join-Path $mnnModel 'config.json') --mnn-max-tokens 48 `
  --tts-model (Join-Path $vitsModel 'model.onnx') `
  --tts-tokens (Join-Path $vitsModel 'tokens.txt') `
  --tts-lexicon (Join-Path $vitsModel 'lexicon.txt') `
  --tts-rule-fars (Join-Path $vitsModel 'rule.far') `
  --tts-threads 2 --events 20 --hz 2 --deadline-ms 3000 --queue 4 --settle-ms 60000 `
  --out (Join-Path $resultDir 'real20.jsonl')
if ($LASTEXITCODE -ne 0) { throw "aimj_bench REAL integration failed with exit $LASTEXITCODE" }
Write-Output "REAL integration smoke OK: $resultDir\real20.jsonl"
