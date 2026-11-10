# External SDK root lives OUTSIDE the repository working tree (third-party sources,
# model weights and prebuilt packages are not committed; hashes and provenance in
# model-sha256.txt). Keep the repo clean for security scanners.
$extern = "$PWD\..\ai_majiang-extern"
New-Item -ItemType Directory -Force "$extern/third_party/downloads", "$extern/third_party/models" | Out-Null
$env:HTTP_PROXY  = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
Push-Location "$extern/third_party/downloads"
# 1) MNN runtime source at the pinned commit
if (-not (Test-Path "..\MNN")) {
  git init ..\MNN; Push-Location ..\MNN
  git remote add origin https://github.com/alibaba/MNN.git
  git fetch --depth 1 origin 24563667d4c923a6f67591a0132141a84b1e2ebb
  git checkout -q FETCH_HEAD; Pop-Location
}
# 2) Official INT4 Pocket model (no requantization)
$m = "https://huggingface.co/taobao-mnn/MNN-Pocket-0.3B-MNN/resolve/main"
New-Item -ItemType Directory -Force "..\models\MNN-Pocket-0.3B-MNN" | Out-Null
foreach ($f in "config.json","llm_config.json","tokenizer.mtok","LICENSE.txt","README.md","llm.mnn","llm.mnn.weight") {
  if (-not (Test-Path "..\models\MNN-Pocket-0.3B-MNN\$f")) {
    curl.exe -sL -o "..\models\MNN-Pocket-0.3B-MNN\$f" "$m/$f"
  }
}
# 3) sherpa-onnx prebuilt win-x64 + vits-icefall-zh-aishell3 model
if (-not (Test-Path "sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2")) {
  curl.exe -sL -o "sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2" "https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2"
}
if (-not (Test-Path "vits-icefall-zh-aishell3.tar.bz2")) {
  curl.exe -sL -o "vits-icefall-zh-aishell3.tar.bz2" "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-icefall-zh-aishell3.tar.bz2"
}
Pop-Location
if (-not (Test-Path "$extern/third_party/sherpa-onnx-extract")) {
  New-Item -ItemType Directory -Force "$extern/sherpa-onnx-extract" | Out-Null
  tar xf "$extern/third_party/downloads/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2" -C "$extern/sherpa-onnx-extract"
}
if (-not (Test-Path "$extern/third_party/models/vits-icefall-zh-aishell3")) {
  tar xf "$extern/third_party/downloads/vits-icefall-zh-aishell3.tar.bz2" -C "$extern/third_party/models"
}
Write-Output "External SDK root ready: $extern"
