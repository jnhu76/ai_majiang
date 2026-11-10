# Stage 1: build official MNN llm_demo/llm_bench with MSVC x64, run real Chinese inference.
# Works from any working directory; paths are derived from this script's location.
# HTTP(S) proxy comes from ambient environment variables; none is hardcoded here.
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$mnn    = Join-Path $extern 'third_party\MNN'
$model  = Join-Path $extern 'third_party\models\MNN-Pocket-0.3B-MNN'
$build  = Join-Path $repo 'build-mnn'
$bench  = Join-Path $repo 'benchmarks\results\mnn-bench'
Write-Output "repo=$repo"
Write-Output "extern=$extern"
if (-not (Test-Path (Join-Path $model 'config.json'))) { throw "model not found at $model - run 00-setup-extern.ps1 first" }

# 1) MNN CPU runtime (configure + build llm_demo / llm_bench)
cmake -S $mnn -B $build -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DMNN_BUILD_LLM=ON -DMNN_LLM_BUILD_DEMO=ON -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF `
  -DMNN_BUILD_TEST=OFF -DMNN_BUILD_BENCHMARK=OFF -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_OPENCV=OFF `
  -DMNN_BUILD_AUDIO=OFF -DMNN_IMGCODECS=OFF -DMNN_BUILD_DIFFUSION=OFF
if ($LASTEXITCODE -ne 0) { throw "MNN cmake configure failed" }
cmake --build $build --config Release --target llm_demo llm_bench
if ($LASTEXITCODE -ne 0) { throw "MNN build failed" }

# 2) Real Chinese prompt through the official llm_demo (UTF-8 file, no BOM)
$llmOut = Join-Path $env:TEMP 'aimj_llm_demo_out.txt'
[System.IO.File]::WriteAllText((Join-Path $env:TEMP 'prompt_zh.txt'), "用一句话介绍麻将的起源。", [System.Text.UTF8Encoding]::new($false))
# Decode child-process stdout as UTF-8 so the Chinese answer stays readable in this process.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PATH = "$build\Release;$env:PATH"
& (Join-Path $build 'Release\llm_demo.exe') (Join-Path $model 'config.json') (Join-Path $env:TEMP 'prompt_zh.txt') | Tee-Object -FilePath $llmOut
if ($LASTEXITCODE -ne 0) { throw "llm_demo failed with exit $LASTEXITCODE" }
if ((Get-Item $llmOut).Length -eq 0) { throw "llm_demo produced no output" }

# 3) Minimal official benchmark, then the thread/token matrix.
New-Item -ItemType Directory -Force $bench | Out-Null
& (Join-Path $build 'Release\llm_bench.exe') -m (Join-Path $model 'config.json') -a cpu -t 8 -pg 64,32 -rep 3
if ($LASTEXITCODE -ne 0) { throw "llm_bench smoke failed" }
& (Join-Path $build 'Release\llm_bench.exe') -m (Join-Path $model 'config.json') -a cpu `
  -t 1,2,4,8 -pg 32,16 -pg 64,32 -pg 128,32 -pg 256,32 -pg 64,64 -rep 3 `
  -j "$bench\llm_bench_matrix.json" > "$bench\llm_bench_matrix.log"
if ($LASTEXITCODE -ne 0) { throw "llm_bench matrix failed" }
Write-Output "llm_bench matrix written to $bench"
