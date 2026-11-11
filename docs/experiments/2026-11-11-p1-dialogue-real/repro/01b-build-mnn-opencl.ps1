# Stage 1b: build the same pinned MNN commit with OpenCL enabled into a separate
# build tree (build-mnn-opencl). The CPU build-mnn tree stays untouched because it
# backs the P0 evidence chain; overwriting its MNN.dll is exactly the incident this
# experiment found (the 2026-11-10 DLL carries zero OpenCL symbols).
# Works from any working directory; HTTP(S) proxy comes from ambient environment
# variables; none is hardcoded here.
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$mnn    = Join-Path $extern 'third_party\MNN'
$build  = Join-Path $repo 'build-mnn-opencl'
$mnnSha = '24563667d4c923a6f67591a0132141a84b1e2ebb'
Write-Output "repo=$repo"
Write-Output "mnn=$mnn"
Write-Output "build=$build"

if (-not (Test-Path $mnn)) { throw "MNN source not found at $mnn - run 00-setup-extern.ps1 first" }
$head = git -C $mnn rev-parse HEAD
if ("$head" -ne $mnnSha) { throw "MNN checkout mismatch: expected $mnnSha, found $head" }

# 1) Configure + build the OpenCL variant next to the CPU tree.
cmake -S $mnn -B $build -G "Visual Studio 17 2022" -A x64 -T "v143,host=x64" `
  -DMNN_OPENCL=ON -DMNN_BUILD_LLM=ON -DMNN_LLM_BUILD_DEMO=OFF `
  -DMNN_BUILD_CONVERTER=OFF -DMNN_BUILD_TRAIN=OFF -DMNN_BUILD_TEST=OFF `
  -DMNN_BUILD_BENCHMARK=OFF -DMNN_BUILD_TOOLS=OFF -DMNN_BUILD_OPENCV=OFF `
  -DMNN_BUILD_AUDIO=OFF -DMNN_IMGCODECS=OFF -DMNN_BUILD_DIFFUSION=OFF
if ($LASTEXITCODE -ne 0) { throw "MNN OpenCL cmake configure failed" }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "MNN OpenCL build failed" }

# 2) Symbol audit: an "OpenCL build" that does not contain the loader entry point
# and OpenCL C kernel sources is a CPU build with a different label.
$dll = Join-Path $build 'Release\MNN.dll'
if (-not (Test-Path $dll)) { throw "MNN.dll missing after build: $dll" }
$text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dll))
foreach ($symbol in 'clCreateCommandQueue', '__kernel') {
    if (-not $text.Contains($symbol)) {
        throw "MNN_OPENCL=ON build lacks symbol '$symbol' in $dll - refusing to label it as OpenCL"
    }
}
$sha = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLower()
Write-Output "MNN.dll OpenCL symbols verified; sha256=$sha"

# 3) ICD enumeration (the DLL can be OpenCL-capable while the machine's ICD is broken).
Add-Type -Namespace AimjProbe -Name OpenCL -MemberDefinition @'
[DllImport("OpenCL.dll")] public static extern int clGetPlatformIDs(uint num_entries, IntPtr[] platforms, out uint num_platforms);
'@
$count = 0
try {
    $rc = [AimjProbe.OpenCL]::clGetPlatformIDs(0, $null, [ref]$count)
} catch {
    throw "OpenCL.dll not loadable: $($_.Exception.Message)"
}
if ($rc -ne 0 -or $count -eq 0) { throw "clGetPlatformIDs rc=$rc platforms=$count - no usable ICD" }
Write-Output "OpenCL ICD OK: rc=$rc platforms=$count"

# 4) Record the hash in the experiment manifest for the pre-run assertion.
# This is the same committed manifest 04-run-p1-matrix.ps1 reads; a missing
# manifest is a hard failure, not a silent "OpenCL build ready".
$manifestPath = Join-Path $repo 'docs\experiments\2026-11-11-p1-dialogue\software-manifest.json'
if (-not (Test-Path $manifestPath)) { throw "manifest not found: $manifestPath" }
$manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $manifest.mnn) {
    $manifest | Add-Member -NotePropertyName mnn -NotePropertyValue ([pscustomobject]@{})
}
if (-not $manifest.mnn.dlls) {
    $manifest.mnn | Add-Member -NotePropertyName dlls -NotePropertyValue ([pscustomobject]@{})
}
if (-not $manifest.mnn.dlls.opencl) {
    $manifest.mnn.dlls | Add-Member -NotePropertyName opencl -NotePropertyValue ([pscustomobject]@{})
}
$manifest.mnn.dlls.opencl.sha256 = $sha
$manifest.mnn.dlls.opencl.opencl_symbols = $true
# PowerShell 5.1 Set-Content -Encoding UTF8 writes a BOM, which breaks JSON
# parsers; write UTF-8 without BOM explicitly.
[System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false))
Write-Output "manifest updated: $manifestPath"
Write-Output "OpenCL build ready: $dll"
