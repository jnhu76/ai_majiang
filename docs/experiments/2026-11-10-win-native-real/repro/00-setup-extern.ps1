# External SDK root lives OUTSIDE the repository working tree (third-party sources,
# model weights and prebuilt packages are not committed; hashes and provenance in
# model-sha256.txt). Keep the repo clean for security scanners.
#
# Version pinning: every asset is fetched at a pinned revision and verified by
# SHA-256 before use. A download failure or hash mismatch aborts the script;
# there is no fallback to a newer asset. An HTTP(S) proxy, if needed, comes from
# the ambient HTTP_PROXY / HTTPS_PROXY environment variables: this script never
# hardcodes a proxy address and never disables TLS certificate verification.
$ErrorActionPreference = 'Stop'

# Repository root (this script is docs/experiments/<date>/repro/NN.ps1) and the
# external SDK root (sibling of the repo, named without any user account).
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$dl     = Join-Path $extern 'third_party\downloads'
$models = Join-Path $extern 'third_party\models'
Write-Output "repo=$repo"
Write-Output "extern=$extern"
New-Item -ItemType Directory -Force $dl, $models | Out-Null

# Download through curl (honors ambient HTTP(S)_PROXY) and verify SHA-256.
# Only byte-identical files are reused; a mismatched file is replaced, never trusted.
function Fetch-Verified([string]$url, [string]$out, [string]$sha256) {
    if (Test-Path $out) {
        if ((Get-FileHash $out -Algorithm SHA256).Hash -ieq $sha256) {
            Write-Output "reuse verified: $out"
            return
        }
        Write-Warning "existing file hash mismatch, re-downloading: $out"
        Remove-Item $out -Force
    }
    curl.exe -fSL --retry 3 -o $out $url
    if ($LASTEXITCODE -ne 0) { throw "download failed (curl exit $LASTEXITCODE): $url" }
    if ((Get-FileHash $out -Algorithm SHA256).Hash -ieq $sha256) {
        Write-Output "downloaded+verified: $out"
    } else {
        Remove-Item $out -Force
        throw "SHA-256 mismatch after download: $out"
    }
}

# Extract an archive only into its destination: missing -> extract; complete -> reuse;
# incomplete -> remove only the archive's own top-level directory ($root) and
# re-extract from the hash-verified archive. $dest may be a shared parent
# (e.g. third_party\models also holds the LLM weights) and is NEVER deleted.
function Ensure-Extracted([string]$archive, [string]$dest, [string]$markerChild, [string[]]$requiredFiles) {
    $root = Join-Path $dest $markerChild
    $complete = ($requiredFiles | ForEach-Object { Test-Path (Join-Path $root $_) }) -notcontains $false
    if ($complete) { Write-Output "reuse complete extraction: $root"; return }
    if (Test-Path -LiteralPath $root) {
        Write-Warning "incomplete extraction, re-extracting from verified archive: $root"
        Remove-Item -LiteralPath $root -Recurse -Force
    }
    New-Item -ItemType Directory -Force $dest | Out-Null
    # Use the Windows system bsdtar explicitly: a GNU tar earlier in PATH
    # (e.g. Git for Windows) rejects "C:\..." in -C as a remote hostname.
    & "$env:SystemRoot\System32\tar.exe" xf $archive -C $dest
    if ($LASTEXITCODE -ne 0) { throw "extraction failed: $archive" }
    if ((($requiredFiles | ForEach-Object { Test-Path (Join-Path $root $_) }) -notcontains $false)) {
        Write-Output "extracted: $root"
    } else { throw "archive did not produce required files under $root" }
}

# 1) MNN runtime source at the pinned commit (never a moving branch).
$mnnSha = '24563667d4c923a6f67591a0132141a84b1e2ebb'
$mnnDir = Join-Path $extern 'third_party\MNN'
if (Test-Path $mnnDir) {
    $head = git -C $mnnDir rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or "$head" -ne $mnnSha) {
        throw "MNN checkout mismatch: $mnnDir is at $head, expected $mnnSha (existing checkout left untouched)"
    }
    Write-Output "reuse pinned MNN @ $mnnSha"
} else {
    git init $mnnDir
    git -C $mnnDir remote add origin https://github.com/alibaba/MNN.git
    git -C $mnnDir fetch --depth 1 origin $mnnSha
    if ($LASTEXITCODE -ne 0) { throw "MNN fetch failed at commit $mnnSha" }
    git -C $mnnDir checkout -q FETCH_HEAD
    Write-Output "cloned pinned MNN @ $mnnSha"
}

# 2) Official INT4 Pocket model at the pinned HuggingFace revision (no /resolve/main).
$hfRev  = '05e05fd35206c6b1f9c662e7c8e56af21cd6b836'
$hfBase = "https://huggingface.co/taobao-mnn/MNN-Pocket-0.3B-MNN/resolve/$hfRev"
$mnnModel = Join-Path $models 'MNN-Pocket-0.3B-MNN'
New-Item -ItemType Directory -Force $mnnModel | Out-Null
# Files required for inference only; LICENSE.txt / README.md are viewable at the
# pinned URL. Hashes match model-sha256.txt in this directory.
Fetch-Verified "$hfBase/config.json"     (Join-Path $mnnModel 'config.json')     '801311d0461dceffb4fe06f71a38b8ffb9616022626109537d3343ecc747619f'
Fetch-Verified "$hfBase/llm_config.json" (Join-Path $mnnModel 'llm_config.json') '64e1b9fc5905fc9e91a8b2007c225b70dc82ffb183ecdb4bfc28e01ac42adc45'
Fetch-Verified "$hfBase/tokenizer.mtok"  (Join-Path $mnnModel 'tokenizer.mtok')  '9f806cad88e8be82e0ca3868903f3528eb1ffcbe4de1437e20549b94fccb4440'
Fetch-Verified "$hfBase/llm.mnn"         (Join-Path $mnnModel 'llm.mnn')         'd42a7d9c7152b45b2b6805f9a5ab6dd95c485ad718be88ec723087d4f5d71d7c'
Fetch-Verified "$hfBase/llm.mnn.weight"  (Join-Path $mnnModel 'llm.mnn.weight')  'c9e0f2950718c057aeb2c20e5b8dd669cd02477c6c355b46d153256a312a248d'

# 3) sherpa-onnx prebuilt win-x64 + vits-icefall-zh-aishell3 archives.
$sherpaPkg = Join-Path $dl 'sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2'
Fetch-Verified 'https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release.tar.bz2' `
    $sherpaPkg 'ce987d5bfca42c74b54f7b3bbaaa8c1febe3c5cd0fea8f94be80ea30e72f1f61'
$vitsPkg = Join-Path $dl 'vits-icefall-zh-aishell3.tar.bz2'
Fetch-Verified 'https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-icefall-zh-aishell3.tar.bz2' `
    $vitsPkg 'ab468db3a3308cdd861495e0db2f25d79418a0c00639f74944c7cdf5dd8c6ec1'

# 4) Extract both archives next to their consumers, with completeness checks.
Ensure-Extracted $sherpaPkg (Join-Path $extern 'third_party\sherpa-onnx-extract') `
    'sherpa-onnx-v2.0.1-win-x64-shared-MD-Release' `
    @('bin\sherpa-onnx-offline-tts.exe', 'lib\sherpa-onnx-c-api.lib', 'include')
Ensure-Extracted $vitsPkg $models 'vits-icefall-zh-aishell3' `
    @('model.onnx', 'tokens.txt', 'lexicon.txt', 'rule.far')

Write-Output "External SDK root ready: $extern"
