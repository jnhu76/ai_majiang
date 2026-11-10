# Stage 1: build official MNN llm_demo/llm_bench with MSVC x64, run real Chinese inference.
# Run from repo root in PowerShell. Proxy is per-process only.
$extern = "$PWD\..\ai_majiang-extern"  # SDK root outside the repo (see 00-setup-extern.ps1)
$env:HTTP_PROXY  = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
$sha = "24563667d4c923a6f67591a0132141a84b1e2ebb"
if (-not (Test-Path "$extern/third_party/MNN")) {
  git init $extern/third_party/MNN
  Push-Location $extern/third_party/MNN
  git remote add origin https://github.com/alibaba/MNN.git
  git fetch --depth 1 origin $sha
  git checkout -q FETCH_HEAD
  Pop-Location
}
cmake -S $extern/third_party/MNN -B build-mnn -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DMNN_BUILD_LLM=ON -DMNN_LLM_BUILD_DEMO=ON -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF `
  -DMNN_BUILD_TEST=OFF -DMNN_BUILD_BENCHMARK=OFF -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_OPENCV=OFF `
  -DMNN_BUILD_AUDIO=OFF -DMNN_IMGCODECS=OFF -DMNN_BUILD_DIFFUSION=OFF
cmake --build build-mnn --config Release --target llm_demo llm_bench
# Real Chinese prompt (UTF-8 file required for llm_demo prompt.txt argument)
[System.IO.File]::WriteAllText("$env:TEMP\prompt_zh.txt", "用一句话介绍麻将的起源。", [System.Text.UTF8Encoding]::new($false))
$env:PATH = "$PWD\build-mnn\Release;$env:PATH"
./build-mnn/Release/llm_demo.exe $extern/third_party/models/MNN-Pocket-0.3B-MNN/config.json "$env:TEMP\prompt_zh.txt"
# Minimal official benchmark, then the thread/token matrix.
mkdir -Force benchmarks/results/mnn-bench | Out-Null
./build-mnn/Release/llm_bench.exe -m $extern/third_party/models/MNN-Pocket-0.3B-MNN/config.json -a cpu -t 8 -pg 64,32 -rep 3
./build-mnn/Release/llm_bench.exe -m $extern/third_party/models/MNN-Pocket-0.3B-MNN/config.json -a cpu `
  -t 1,2,4,8 -pg 32,16 -pg 64,32 -pg 128,32 -pg 256,32 -pg 64,64 -rep 3 `
  -j benchmarks/results/mnn-bench/llm_bench_matrix.json > benchmarks/results/mnn-bench/llm_bench_matrix.log
