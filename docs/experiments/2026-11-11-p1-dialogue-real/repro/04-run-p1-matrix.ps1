# Stage 4: single orchestration point for the P1 dialogue/TTS/OpenCL experiment.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File 04-run-p1-matrix.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File 04-run-p1-matrix.ps1 -SmokeOnly -ResultsRoot tmp/p1-smoke
#
# Real mode builds/uses build-real-cpu (build-mnn CPU DLL) and build-real-opencl
# (build-mnn-opencl OpenCL DLL), stages each pair in its own directory so a CPU
# request can never load the OpenCL DLL by accident, asserts the staged MNN.dll
# hash against software-manifest.json before every run, runs calibration before
# the batch, then three configs, the TTS battery, six scenarios and A/B/C hybrid
# arms. All stdout/stderr go to console.out (the repo ignores *.log globally).
# Native PowerShell only; no WSL/Linux path is used.
param(
    [string]$RunId = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string[]]$Configs = @('cpu4', 'cpu8', 'opencl'),
    [int]$Rounds = 5,
    [string]$ResultsRoot = 'benchmarks/results/p1',
    [switch]$SmokeOnly,
    [switch]$SkipBuild,
    [switch]$AllowUnpinnedDll
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$tp = Join-Path $extern 'third_party'
$mnn = Join-Path $tp 'MNN'
$mnnModel = Join-Path $tp 'models\MNN-Pocket-0.3B-MNN'
$sherpaRoot = Join-Path $tp 'sherpa-onnx-extract\sherpa-onnx-v2.0.1-win-x64-shared-MD-Release'
$vitsModel = Join-Path $tp 'models\vits-icefall-zh-aishell3'
$expDir = Join-Path $repo 'docs\experiments\2026-11-11-p1-dialogue'
$manifestPath = Join-Path $expDir 'software-manifest.json'
if (-not [System.IO.Path]::IsPathRooted($ResultsRoot)) { $ResultsRoot = Join-Path $repo $ResultsRoot }
# Smoke runs keep their scratch provenance under the (gitignored) results root;
# only real runs write sanitized evidence under docs/.../runs/. The transcript
# always stays in the gitignored results tree: its header carries the local
# user, host and host application and must never enter committed evidence.
$runDir = if ($SmokeOnly) { Join-Path $ResultsRoot "runs\$RunId" }
          else { Join-Path $expDir "runs\$RunId" }
New-Item -ItemType Directory -Force $runDir | Out-Null
New-Item -ItemType Directory -Force $ResultsRoot | Out-Null
$logDir = Join-Path $ResultsRoot "runs\$RunId"
New-Item -ItemType Directory -Force $logDir | Out-Null
$runnerLog = Join-Path $logDir 'runner.out'
try { Start-Transcript -Path $runnerLog -Force | Out-Null }
catch { Write-Warning "transcript unavailable: $($_.Exception.Message)" }

Write-Output "repo=$repo"
Write-Output "results=$ResultsRoot"
Write-Output "run_id=$RunId"

if (-not (Test-Path $manifestPath)) {
    throw "manifest not found: $manifestPath (the committed P1 manifest must exist before a run)"
}
$manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$configMap = @{
    cpu4   = @{ backend = 'cpu';    threads = 4; build = 'build-real-cpu';    mnn = 'build-mnn';        mnnKey = 'cpu' }
    cpu8   = @{ backend = 'cpu';    threads = 8; build = 'build-real-cpu';    mnn = 'build-mnn';        mnnKey = 'cpu' }
    opencl = @{ backend = 'opencl'; threads = 0; build = 'build-real-opencl'; mnn = 'build-mnn-opencl'; mnnKey = 'opencl' }
}

function Write-JsonFile([string]$Path, $Value) {
    # PowerShell 5.1 Set-Content -Encoding UTF8 adds a BOM; evidence JSON must
    # stay machine-readable by plain json.load.
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10),
        [System.Text.UTF8Encoding]::new($false))
}
function Get-Sha256([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLower()
}
function Convert-ToRepoRelative([string]$Path) {
    # Committed evidence uses placeholders instead of the local absolute paths
    # that provenance/opencl records otherwise inherit from this machine.
    if (-not $Path) { return $Path }
    $full = [System.IO.Path]::GetFullPath($Path)
    $roots = @(
        @{ Root = [System.IO.Path]::GetFullPath($repo).TrimEnd('\'); Label = '<repo>' },
        @{ Root = [System.IO.Path]::GetFullPath($extern).TrimEnd('\'); Label = '<extern>' }
    )
    foreach ($item in $roots) {
        if ($full -ieq $item.Root) { return $item.Label }
        $prefix = $item.Root + '\'
        if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $item.Label + '/' + $full.Substring($prefix.Length).Replace('\', '/')
        }
    }
    return '<external>/' + [System.IO.Path]::GetFileName($Path)
}
function Quote-Arg([string]$Value) {
    if ($Value -match '[\s"]') { return '"' + ($Value -replace '"', '\"') + '"' }
    return $Value
}
function Read-Jsonl([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    return @(Get-Content -Path $Path -Encoding UTF8 |
        Where-Object { $_.Trim().Length -gt 0 } |
        ForEach-Object { $_ | ConvertFrom-Json })
}
function Get-RunHeader([string]$Path) {
    Read-Jsonl $Path | Where-Object { $_.type -eq 'run' } | Select-Object -First 1
}
function Invoke-Probe {
    param(
        [string]$Exe,
        [string]$WorkDir,
        [string[]]$ProbeArgs,
        [string]$ConsoleOut,
        [string]$Label,
        [switch]$SampleGpu
    )
    $consoleDir = Split-Path -Parent $ConsoleOut
    New-Item -ItemType Directory -Force $consoleDir | Out-Null
    $errorOut = [System.IO.Path]::ChangeExtension($ConsoleOut, '.err.out')
    $argString = ($ProbeArgs | ForEach-Object { Quote-Arg $_ }) -join ' '
    # Start-Process -PassThru does not reliably expose ExitCode under PowerShell
    # 5.1 here, so drive System.Diagnostics.Process directly and read both pipes
    # asynchronously to avoid a full-buffer deadlock.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $argString
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) { throw "failed to start $Exe" }
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $gpuCsv = $null
    $job = $null
    if ($SampleGpu) {
        $gpuCsv = [System.IO.Path]::ChangeExtension($ConsoleOut, '.gpu.csv')
        $job = Start-Job -FilePath (Join-Path $PSScriptRoot 'gpu-guard.ps1') `
            -ArgumentList $proc.Id, $gpuCsv, 1000
    }
    $proc.WaitForExit()
    [System.IO.File]::WriteAllText($ConsoleOut, $stdoutTask.Result, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($errorOut, $stderrTask.Result, [System.Text.UTF8Encoding]::new($false))
    $exitCode = $proc.ExitCode
    if ($job) {
        Wait-Job $job | Out-Null
        Receive-Job $job | Out-Null
        Remove-Job $job
    }
    Write-Output ("[{0}] exit={1} console={2}" -f $Label, $exitCode, $ConsoleOut)
    if ($exitCode -notin @(0, 2)) {
        throw "$Label failed with exit $exitCode; see $ConsoleOut and $errorOut"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; GpuCsv = $gpuCsv; ErrorOut = $errorOut }
}
function Convert-EvidenceText([string]$Text) {
    # Run headers and console lines carry absolute model/DLL paths. Replace both
    # the raw and JSON-escaped/forward-slash forms before evidence is committed.
    if (-not $Text) { return $Text }
    $roots = @(
        @{ Root = [System.IO.Path]::GetFullPath($repo).TrimEnd('\'); Label = '<repo>' },
        @{ Root = [System.IO.Path]::GetFullPath($extern).TrimEnd('\'); Label = '<extern>' }
    )
    foreach ($item in $roots) {
        foreach ($variant in @($item.Root, ($item.Root -replace '\\', '\\'), ($item.Root -replace '\\', '/'))) {
            $Text = $Text.Replace($variant, $item.Label)
        }
    }
    return $Text
}
function Save-Evidence {
    param([string]$EvidenceDir, [string]$Jsonl, [string]$ConsoleOut, [string]$GpuCsv, [string]$ErrorOut)
    New-Item -ItemType Directory -Force $EvidenceDir | Out-Null
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    if (Test-Path $Jsonl) {
        $text = Get-Content $Jsonl -Raw -Encoding UTF8
        [System.IO.File]::WriteAllText((Join-Path $EvidenceDir 'results.jsonl'), (Convert-EvidenceText $text), $utf8)
    }
    if (Test-Path $ConsoleOut) {
        $text = Get-Content $ConsoleOut -Raw -Encoding UTF8
        [System.IO.File]::WriteAllText((Join-Path $EvidenceDir 'console.out'), (Convert-EvidenceText $text), $utf8)
    }
    if ($ErrorOut -and (Test-Path $ErrorOut)) {
        $text = Get-Content $ErrorOut -Raw -Encoding UTF8
        [System.IO.File]::WriteAllText((Join-Path $EvidenceDir 'console.err.out'), (Convert-EvidenceText $text), $utf8)
    }
    if ($GpuCsv -and (Test-Path $GpuCsv)) { Copy-Item $GpuCsv (Join-Path $EvidenceDir 'gpu.csv') -Force }
}
function New-CalibrationCases([string]$OutTsv) {
    $lines = Get-Content -Path (Join-Path $repo 'benchmarks\p1_dialogue_cases.tsv') -Encoding UTF8
    $data = @($lines | Where-Object { $_.Trim().Length -gt 0 -and -not $_.StartsWith('#') } | Select-Object -First 1)
    if ($data.Count -eq 0) { throw 'no dialogue fixture available for calibration' }
    $header = @(
        '# calibration subset: one public event fixture only',
        '# UTF-8 TSV, no header. Columns: case_id, speaker(1..3), category, prompt, expected_review_hint, forbidden_review_hint, required_kw, forbidden_kw, persona_key, fact, history, structured'
    )
    # No BOM: the C++ TSV loader treats a UTF-8 BOM as part of the line and
    # would stop recognizing the comment header.
    [System.IO.File]::WriteAllText($OutTsv, (($header + $data) -join [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}
function Configure-Integration([string]$BuildDir, [string]$MnnBuildDir) {
    $mnnEngineInc = Join-Path $mnn 'transformers\llm\engine\include'
    $mnnCoreInc = Join-Path $mnn 'include'
    $mnnLib = Join-Path (Join-Path $repo $MnnBuildDir) 'Release\MNN.lib'
    $sherpaInc = Join-Path $sherpaRoot 'include'
    $sherpaLib = Join-Path $sherpaRoot 'lib\sherpa-onnx-c-api.lib'
    foreach ($path in @($mnnLib, (Join-Path $mnnEngineInc 'llm\llm.hpp'), $sherpaLib)) {
        if (-not (Test-Path $path)) { throw "missing integration dependency: $path" }
    }
    cmake -S $repo -B (Join-Path $repo $BuildDir) -G 'Visual Studio 17 2022' -A x64 -T 'v143,host=x64' `
        -DAIMJ_WITH_MNN=ON -DAIMJ_MNN_INCLUDE_DIR="$mnnEngineInc;$mnnCoreInc" `
        -DAIMJ_MNN_LIBRARIES="$mnnLib" -DAIMJ_WITH_SHERPA=ON `
        -DAIMJ_SHERPA_INCLUDE_DIR="$sherpaInc" -DAIMJ_SHERPA_LIBRARIES="$sherpaLib"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $BuildDir" }
    cmake --build (Join-Path $repo $BuildDir) --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $BuildDir" }
    ctest --test-dir (Join-Path $repo $BuildDir) -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "ctest failed for $BuildDir" }
}
function New-Stage([string]$Config) {
    $info = $configMap[$Config]
    $stage = Join-Path $runDir "stage\$Config"
    New-Item -ItemType Directory -Force $stage | Out-Null
    $exeSrc = Join-Path (Join-Path $repo $info.build) 'Release\aimj_dialogue_probe.exe'
    $mnnDll = Join-Path (Join-Path $repo $info.mnn) 'Release\MNN.dll'
    if (-not (Test-Path $exeSrc)) { throw "missing probe for ${Config}: $exeSrc" }
    if (-not (Test-Path $mnnDll)) { throw "missing MNN.dll for ${Config}: $mnnDll (run 01b for opencl)" }
    Copy-Item $exeSrc $stage -Force
    Copy-Item $mnnDll $stage -Force
    Copy-Item (Join-Path $sherpaRoot 'lib\*.dll') $stage -Force
    Copy-Item (Join-Path $sherpaRoot 'bin\onnxruntime*.dll') $stage -Force
    $stagedSha = Get-Sha256 (Join-Path $stage 'MNN.dll')
    $expected = $manifest.mnn.dlls.$($info.mnnKey).sha256
    if ($expected) {
        if ($expected -ne $stagedSha) {
            throw "MNN.dll hash mismatch for ${Config}: expected $expected, staged $stagedSha"
        }
        Write-Output "[hash] $Config MNN.dll matches pinned $expected"
    } elseif (-not $AllowUnpinnedDll) {
        $manifest.mnn.dlls.$($info.mnnKey).sha256 = $stagedSha
        Write-JsonFile $manifestPath $manifest
        Write-Output "[hash] pinned first observation for ${Config}: $stagedSha"
    } else {
        Write-Output "[warn] $Config MNN.dll is not pinned; -AllowUnpinnedDll was passed"
    }
    return $stage
}
function Write-Provenance([string]$Phase) {
    $provenance = [ordered]@{
        experiment = '2026-11-11-p1-dialogue-real'
        run_id     = $RunId
        phase      = $Phase
        generated  = (Get-Date).ToString('o')
        machine    = [ordered]@{
            cpu           = (Get-CimInstance Win32_Processor).Name
            memory_bytes  = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
            os            = (Get-CimInstance Win32_OperatingSystem).Caption
            build_number  = (Get-CimInstance Win32_OperatingSystem).BuildNumber
        }
        probe      = [ordered]@{}
        mnn_dlls   = [ordered]@{}
        model      = [ordered]@{}
        sherpa     = [ordered]@{}
        vits       = [ordered]@{}
        fixtures   = [ordered]@{}
        scripts    = [ordered]@{}
        wav_sha256 = [ordered]@{}
    }
    foreach ($config in $configMap.Keys) {
        $exe = Join-Path (Join-Path $repo $configMap[$config].build) 'Release\aimj_dialogue_probe.exe'
        $dll = Join-Path (Join-Path $repo $configMap[$config].mnn) 'Release\MNN.dll'
        $provenance.probe[$config] = @{ path = (Convert-ToRepoRelative $exe); sha256 = Get-Sha256 $exe }
        $provenance.mnn_dlls[$config] = @{ path = (Convert-ToRepoRelative $dll); sha256 = Get-Sha256 $dll }
    }
    foreach ($file in @('config.json', 'llm.mnn', 'llm.mnn.weight', 'tokenizer.mtok')) {
        $path = Join-Path $mnnModel $file
        $provenance.model[$file] = @{ path = (Convert-ToRepoRelative $path); sha256 = Get-Sha256 $path }
    }
    $provenance.sherpa = @{
        root = (Convert-ToRepoRelative $sherpaRoot)
        version = 'v2.0.1'
        c_api_dll_sha256 = Get-Sha256 (Join-Path $sherpaRoot 'lib\sherpa-onnx-c-api.dll')
    }
    foreach ($file in @('model.onnx', 'tokens.txt', 'lexicon.txt', 'rule.far')) {
        $path = Join-Path $vitsModel $file
        $provenance.vits[$file] = @{ path = (Convert-ToRepoRelative $path); sha256 = Get-Sha256 $path }
    }
    foreach ($file in @('benchmarks\p1_dialogue_cases.tsv', 'benchmarks\p1_tts_lines.tsv', 'benchmarks\p1_bank_manifest.tsv')) {
        $path = Join-Path $repo $file
        $provenance.fixtures[$file] = @{ sha256 = Get-Sha256 $path; bytes = if (Test-Path $path) { (Get-Item $path).Length } else { 0 } }
    }
    foreach ($script in @('01b-build-mnn-opencl.ps1', 'gpu-guard.ps1', '04-run-p1-matrix.ps1')) {
        $path = Join-Path $PSScriptRoot $script
        $provenance.scripts[$script] = Get-Sha256 $path
    }
    # Every produced WAV is hashed; bytes stay on this machine.
    $wavRoots = @()
    $ttsDir = Join-Path $ResultsRoot 'audio\tts_real'
    if (Test-Path $ttsDir) { $wavRoots += $ttsDir }
    $dialogueRoot = Join-Path $ResultsRoot 'dialogue'
    if (Test-Path $dialogueRoot) {
        $wavRoots += @(Get-ChildItem -Path $dialogueRoot -Directory -Filter "*$RunId*" |
            ForEach-Object { $_.FullName })
    }
    foreach ($root in $wavRoots) {
        foreach ($wav in @(Get-ChildItem -Path $root -Filter '*.wav' -File)) {
            $key = "$($wav.Directory.Name)/$($wav.Name)"
            $provenance.wav_sha256[$key] = Get-Sha256 $wav.FullName
        }
    }
    $path = Join-Path $runDir 'provenance.json'
    Write-JsonFile $path $provenance
    Write-Output "provenance -> $path ($($provenance.wav_sha256.Count) WAV hashes)"
}

# ---------------------------------------------------------------- smoke path
if ($SmokeOnly) {
    $exe = Join-Path $repo 'build-real\Release\aimj_dialogue_probe.exe'
    if (-not (Test-Path $exe)) { throw "missing $exe - build build-real first" }
    $smokeRoot = Join-Path $ResultsRoot 'smoke'
    New-Item -ItemType Directory -Force $smokeRoot | Out-Null
    $cases = Join-Path $repo 'benchmarks\p1_dialogue_cases.tsv'
    $dialogueId = "$RunId-smoke-dialogue"
    $dialogueConsole = Join-Path $smokeRoot "dialogue\$dialogueId\console.out"
    $result = Invoke-Probe -Exe $exe -WorkDir (Split-Path -Parent $exe) -Label 'smoke-dialogue' `
        -ConsoleOut $dialogueConsole -ProbeArgs @(
            '--cases', $cases, '--output', (Join-Path $smokeRoot 'dialogue'), '--run-id', $dialogueId,
            '--llm', 'mock', '--tts', 'mock', '--strategies', 'P0,P1,P2,P3', '--rounds', '1',
            '--queue', '4', '--force')
    $ttsId = "$RunId-smoke-tts"
    $ttsConsole = Join-Path $smokeRoot "tts\$ttsId\console.out"
    $result = Invoke-Probe -Exe $exe -WorkDir (Split-Path -Parent $exe) -Label 'smoke-tts' `
        -ConsoleOut $ttsConsole -ProbeArgs @(
            '--battery', 'tts_lines', '--tts-lines', (Join-Path $repo 'benchmarks\p1_tts_lines.tsv'),
            '--output', (Join-Path $smokeRoot 'tts'), '--run-id', $ttsId,
            '--llm', 'mock', '--tts', 'mock', '--force')
    $s6Console = Join-Path $smokeRoot "scenario\$RunId-s6\console.out"
    $result = Invoke-Probe -Exe $exe -WorkDir (Split-Path -Parent $exe) -Label 'smoke-s6' `
        -ConsoleOut $s6Console -ProbeArgs @(
            '--battery', 'scenarios', '--scenario', 's6', '--cases', $cases,
            '--output', (Join-Path $smokeRoot 'scenario'),
            '--run-id', "$RunId-s6", '--llm', 'mock', '--tts', 'mock', '--queue', '2',
            '--inject-llm-error-after', '0', '--force')
    & python -I (Join-Path $repo 'benchmarks\p1_analyze.py') --root $ResultsRoot
    if ($LASTEXITCODE -ne 0) { throw 'p1_analyze.py failed on smoke output' }
    Write-Provenance 'smoke'
    try { Stop-Transcript | Out-Null } catch { }
    Write-Output 'SMOKE OK: harness plumbing exercised with mock providers only'
    exit 0
}

# --------------------------------------------------------------- real matrix
if (-not $SkipBuild) {
    foreach ($config in $Configs) {
        $info = $configMap[$config]
        if ($config -eq 'opencl') { continue } # built below, wrapped so a CPU run can still proceed
        Configure-Integration $info.build $info.mnn
    }
}
$stages = @{}
foreach ($config in $Configs) {
    if ($config -notin @('cpu4', 'cpu8', 'opencl')) { throw "unknown config: $config" }
}
$openclNote = $null
foreach ($config in $Configs) {
    if ($config -eq 'opencl') {
        try {
            if (-not $SkipBuild) {
                if (-not (Test-Path (Join-Path $repo 'build-mnn-opencl\Release\MNN.dll'))) {
                    & (Join-Path $PSScriptRoot '01b-build-mnn-opencl.ps1')
                    if ($LASTEXITCODE -ne 0) { throw '01b-build-mnn-opencl.ps1 failed' }
                    # 01b pins the freshly built hash; reload so New-Stage does not
                    # compare it against the pre-build manifest.
                    $manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
                }
                Configure-Integration $configMap[$config].build $configMap[$config].mnn
            }
            $stages[$config] = New-Stage $config
        } catch {
            $openclNote = "NO-GO: $($_.Exception.Message)"
            Write-Output "[opencl] $openclNote"
            Write-JsonFile (Join-Path $runDir 'opencl-unavailable.json') (@{ status = 'NO-GO'; reason = $openclNote; at = (Get-Date).ToString('o') })
        }
        continue
    }
    $stages[$config] = New-Stage $config
}
if ($stages.Count -eq 0) { throw 'no runnable configuration' }

# Fixture paths and provider arguments shared by real runs.
$casesPath = Join-Path $repo 'benchmarks\p1_dialogue_cases.tsv'
$ttsLinesPath = Join-Path $repo 'benchmarks\p1_tts_lines.tsv'
$bankManifest = Join-Path $repo 'benchmarks\p1_bank_manifest.tsv'
$providerArgs = @(
    '--llm', 'mnn', '--tts', 'sherpa',
    '--llm-sdk-label', 'MNN 24563667 CPU/OpenCL build', '--tts-sdk-label', 'sherpa-onnx v2.0.1',
    '--mnn-config', (Join-Path $mnnModel 'config.json'),
    '--tts-model', (Join-Path $vitsModel 'model.onnx'),
    '--tts-tokens', (Join-Path $vitsModel 'tokens.txt'),
    '--tts-lexicon', (Join-Path $vitsModel 'lexicon.txt'),
    '--tts-rule-fars', (Join-Path $vitsModel 'rule.far'),
    '--tts-threads', '2'
)
$ttsArgs = @(
    '--llm', 'mnn', '--tts', 'sherpa',
    '--llm-sdk-label', 'MNN 24563667 CPU/OpenCL build', '--tts-sdk-label', 'sherpa-onnx v2.0.1',
    '--mnn-config', (Join-Path $mnnModel 'config.json'), '--mnn-backend', 'cpu', '--mnn-threads', '4',
    '--tts-model', (Join-Path $vitsModel 'model.onnx'),
    '--tts-tokens', (Join-Path $vitsModel 'tokens.txt'),
    '--tts-lexicon', (Join-Path $vitsModel 'lexicon.txt'),
    '--tts-rule-fars', (Join-Path $vitsModel 'rule.far'),
    '--tts-threads', '2'
)

Write-Provenance 'before-runs'

# Calibration first: it proves the truncated/error semantics before bulk data.
$calibrationCases = Join-Path $runDir 'calibration-case.tsv'
New-CalibrationCases $calibrationCases
$calibrationDir = Join-Path $ResultsRoot 'calibration'
$calibrationConfig = if ($stages.ContainsKey('cpu4')) { 'cpu4' } else { @($stages.Keys)[0] }
$calibrationStage = $stages[$calibrationConfig]
function Invoke-CalibrationRun([string]$Name, [string[]]$ExtraArgs, [scriptblock]$Check) {
    $id = "$RunId-calib-$Name"
    $jsonl = Join-Path $calibrationDir "$id\results.jsonl"
    $console = Join-Path $calibrationDir "$id\console.out"
    $probeArgs = @('--battery', 'dialogue', '--cases', $calibrationCases,
        '--output', $calibrationDir, '--run-id', $id, '--llm', 'mnn', '--tts', 'mock',
        '--llm-sdk-label', 'MNN 24563667 CPU/OpenCL build',
        '--mnn-config', (Join-Path $mnnModel 'config.json'),
        '--mnn-backend', $configMap[$calibrationConfig].backend,
        '--mnn-threads', "$($configMap[$calibrationConfig].threads)",
        '--strategies', 'P1', '--rounds', '1', '--queue', '2', '--deadline-ms', '120000',
        '--config-name', $calibrationConfig, '--force') + $ExtraArgs
    $result = Invoke-Probe -Exe (Join-Path $calibrationStage 'aimj_dialogue_probe.exe') `
        -WorkDir $calibrationStage -ProbeArgs $probeArgs -ConsoleOut $console -Label "calibration-$Name"
    Save-Evidence -EvidenceDir (Join-Path $runDir "calibration-$Name") -Jsonl $jsonl `
        -ConsoleOut $console -ErrorOut $result.ErrorOut -GpuCsv $result.GpuCsv
    $rows = Read-Jsonl $jsonl
    $case = $rows | Where-Object { $_.type -eq 'case' } | Select-Object -First 1
    if (-not $case) { throw "calibration $Name produced no case row" }
    & $Check $case
    Write-Output ("[calibration] {0}: status={1} status_code={2} truncated={3} injected={4}" -f `
        $Name, $case.status, $case.status_code, $case.truncated, $case.injected_fault)
}
Invoke-CalibrationRun 'max4' @('--max-tokens', '4') {
    param($case)
    if ($case.status_code -ne 0 -or -not $case.truncated) {
        throw "max-tokens=4 expected status RUNNING(0) + truncated=true, got status_code=$($case.status_code) truncated=$($case.truncated) status=$($case.status)"
    }
}
Invoke-CalibrationRun 'eos64' @('--max-tokens', '64') {
    param($case)
    if ($case.status_code -ne 1 -or $case.truncated) {
        throw "max-tokens=64 expected NORMAL_FINISHED(1) + truncated=false, got status_code=$($case.status_code) truncated=$($case.truncated) status=$($case.status)"
    }
}
Invoke-CalibrationRun 'inject-error' @('--max-tokens', '64', '--inject-llm-error-after', '0') {
    param($case)
    if ($case.status -ne 'error' -or $case.status_code -ne 4 -or $case.truncated -ne $false -or
        $case.usage_valid -ne $false -or $case.injected_fault -ne $true) {
        throw "injected error expected status=error, status_code=4, truncated=false, usage_valid=false, injected_fault=true; got status=$($case.status) status_code=$($case.status_code) truncated=$($case.truncated) usage_valid=$($case.usage_valid) injected=$($case.injected_fault)"
    }
}
Write-Output '[calibration] all three semantic checks passed'

# Three configurations: same fixtures, same rounds, one DLL each.
$runHeaders = @{}
foreach ($config in @($Configs)) {
    if (-not $stages.ContainsKey($config)) { continue }
    $stage = $stages[$config]
    $id = "$RunId-$config"
    $jsonl = Join-Path $ResultsRoot "dialogue\$id\results.jsonl"
    $console = Join-Path $ResultsRoot "dialogue\$id\console.out"
    $probeArgs = @('--battery', 'dialogue', '--cases', $casesPath,
        '--output', (Join-Path $ResultsRoot 'dialogue'), '--run-id', $id,
        '--mnn-backend', $configMap[$config].backend,
        '--mnn-threads', "$($configMap[$config].threads)",
        '--max-tokens', '64', '--strategies', 'P0,P1,P2,P3',
        '--rounds', "$Rounds", '--warmup-rounds', '1', '--queue', '4', '--deadline-ms', '4000',
        '--config-name', $config, '--force') + $providerArgs
    try {
        # MNN flushes its OpenCL kernel cache on process exit, so a stale file
        # from an earlier run must be removed before the run being judged.
        if ($config -eq 'opencl') {
            Remove-Item (Join-Path $stage 'mnn_cachefile.bin') -Force -ErrorAction SilentlyContinue
        }
        $result = Invoke-Probe -Exe (Join-Path $stage 'aimj_dialogue_probe.exe') -WorkDir $stage `
            -ProbeArgs $probeArgs -ConsoleOut $console -Label "dialogue-$config" `
            -SampleGpu:($config -eq 'opencl')
        Save-Evidence -EvidenceDir (Join-Path $runDir "dialogue-$config") -Jsonl $jsonl `
            -ConsoleOut $console -ErrorOut $result.ErrorOut -GpuCsv $result.GpuCsv
        $runHeaders[$config] = Get-RunHeader $jsonl
        $header = $runHeaders[$config]
        Write-Output ("[dialogue] {0}: effective={1} opencl_symbols={2} icd={3}" -f `
            $config, $header.mnn_backend_effective, $header.mnn_dll_opencl_symbols, $header.opencl_icd_available)
        if ($config -eq 'opencl') {
            # Post-exit evidence: this run created the OpenCL kernel cache.
            $cacheFile = Join-Path $stage 'mnn_cachefile.bin'
            $cachePresent = Test-Path $cacheFile
            $gpuUtil = 0.0
            if ($result.GpuCsv -and (Test-Path $result.GpuCsv)) {
                $samples = @(Import-Csv $result.GpuCsv)
                if ($samples.Count -gt 0) {
                    $gpuUtil = ($samples | ForEach-Object { [double]$_.gpu_util_pct } |
                        Measure-Object -Maximum).Maximum
                }
            }
            $openclEvidence = [ordered]@{
                config                 = $config
                backend_effective      = $header.mnn_backend_effective
                mnn_dll_path           = (Convert-ToRepoRelative $header.mnn_dll_path)
                mnn_dll_sha256         = $header.mnn_dll_sha256
                mnn_dll_opencl_symbols = $header.mnn_dll_opencl_symbols
                icd_available          = $header.opencl_icd_available
                icd_detail             = $header.opencl_icd_detail
                cache_file             = (Convert-ToRepoRelative $cacheFile)
                cache_present          = $cachePresent
                cache_bytes            = if ($cachePresent) { (Get-Item $cacheFile).Length } else { 0 }
                cache_sha256           = Get-Sha256 $cacheFile
                gpu_util_max_pct       = $gpuUtil
                gpu_samples            = (Convert-ToRepoRelative $result.GpuCsv)
                checked_at             = (Get-Date).ToString('o')
            }
            Write-JsonFile (Join-Path $runDir "opencl-evidence-$config.json") $openclEvidence
            Write-Output ("[opencl] cache_present={0} cache_bytes={1} gpu_util_max={2}" -f `
                $cachePresent, $openclEvidence.cache_bytes, $gpuUtil)
            if ($header.mnn_backend_effective -ne 'opencl_verified' -or -not $cachePresent) {
                $openclNote = "NO-GO: effective=$($header.mnn_backend_effective) cache_present=$cachePresent"
                Write-JsonFile (Join-Path $runDir 'opencl-unavailable.json') (@{ status = 'NO-GO'; reason = $openclNote; at = (Get-Date).ToString('o') })
            }
        }
    } catch {
        if ($config -eq 'opencl') {
            $openclNote = "NO-GO: $($_.Exception.Message)"
            Write-Output "[opencl] $openclNote"
            Write-JsonFile (Join-Path $runDir 'opencl-unavailable.json') (@{ status = 'NO-GO'; reason = $openclNote; at = (Get-Date).ToString('o') })
            continue
        }
        throw
    }
}

# TTS battery (real sherpa, 37 fixtures). The committed run header records
# tts_sids=[8,9,10]; pass the same mapping so a reproduction keeps the voices.
$ttsStage = if ($stages.ContainsKey('cpu4')) { $stages['cpu4'] } else { $stages[@($stages.Keys)[0]] }
$ttsId = 'tts_real'
$ttsJsonl = Join-Path $ResultsRoot "audio\$ttsId\results.jsonl"
$ttsConsole = Join-Path $ResultsRoot "audio\$ttsId\console.out"
$ttsResult = Invoke-Probe -Exe (Join-Path $ttsStage 'aimj_dialogue_probe.exe') -WorkDir $ttsStage `
    -ProbeArgs (@('--battery', 'tts_lines', '--tts-lines', $ttsLinesPath,
        '--output', (Join-Path $ResultsRoot 'audio'), '--run-id', $ttsId,
        '--tts-sids', '8,9,10', '--force') + $ttsArgs) `
    -ConsoleOut $ttsConsole -Label 'tts-battery'
Save-Evidence -EvidenceDir (Join-Path $runDir 'tts-battery') -Jsonl $ttsJsonl `
    -ConsoleOut $ttsConsole -ErrorOut $ttsResult.ErrorOut
$ttsRate = (Read-Jsonl $ttsJsonl | Where-Object { $_.type -eq 'tts_line' -and $_.status -eq 'ok' } |
    Select-Object -First 1).sample_rate
Write-Output "[tts] battery sample_rate=$ttsRate (from JSONL, not assumed)"

# Six scenarios on the primary CPU configuration (interaction aggregation is per
# scenario directory, so mixing configs there would corrupt the schema).
$scenarioStage = if ($stages.ContainsKey('cpu4')) { $stages['cpu4'] } else { $stages[@($stages.Keys)[0]] }
$scenarioArgs = @('--battery', 'scenarios', '--cases', $casesPath,
    '--llm-sdk-label', 'MNN 24563667 CPU/OpenCL build', '--tts-sdk-label', 'sherpa-onnx v2.0.1',
    '--output', (Join-Path $ResultsRoot 'interaction'),
    '--llm', 'mnn', '--tts', 'sherpa',
    '--mnn-config', (Join-Path $mnnModel 'config.json'), '--mnn-backend', 'cpu', '--mnn-threads', '4',
    '--tts-model', (Join-Path $vitsModel 'model.onnx'),
    '--tts-tokens', (Join-Path $vitsModel 'tokens.txt'),
    '--tts-lexicon', (Join-Path $vitsModel 'lexicon.txt'),
    '--tts-rule-fars', (Join-Path $vitsModel 'rule.far'),
    '--tts-threads', '2', '--force')
foreach ($scenario in @('s1', 's2', 's3', 's4', 's5', 's6')) {
    $id = $scenario
    $jsonl = Join-Path $ResultsRoot "interaction\$id\results.jsonl"
    $console = Join-Path $ResultsRoot "interaction\$id\console.out"
    $extra = @('--scenario', $scenario, '--run-id', $id)
    if ($scenario -eq 's4') { $extra += @('--queue', '2', '--deadline-ms', '5') }
    elseif ($scenario -eq 's5') { $extra += @('--queue', '8') }
    elseif ($scenario -eq 's6') { $extra += @('--queue', '2', '--inject-llm-error-after', '0') }
    else { $extra += @('--queue', '2') }
    $result = Invoke-Probe -Exe (Join-Path $scenarioStage 'aimj_dialogue_probe.exe') `
        -WorkDir $scenarioStage -ProbeArgs ($scenarioArgs + $extra) -ConsoleOut $console -Label "scenario-$scenario"
    Save-Evidence -EvidenceDir (Join-Path $runDir "scenario-$scenario") -Jsonl $jsonl `
        -ConsoleOut $console -ErrorOut $result.ErrorOut
    $summary = Read-Jsonl $jsonl | Where-Object { $_.type -eq 'scenario' } | Select-Object -Last 1
    Write-Output ("[scenario] {0}: rows={1} ok={2} rejected={3} stale={4} late_violation={5} stale_audio={6}" -f `
        $scenario, $summary.rows, $summary.ok, $summary.submit_rejected, $summary.runtime_stale, `
        $summary.late_publish_violation, $summary.stale_audio_violation)
}

# A/B/C hybrid arms on the same six-scenario fixture set (free/template/cache).
$hybridBase = @('--battery', 'hybrid', '--output', (Join-Path $ResultsRoot 'hybrid'),
    '--llm-sdk-label', 'MNN 24563667 CPU/OpenCL build', '--tts-sdk-label', 'sherpa-onnx v2.0.1',
    '--llm', 'mnn', '--tts', 'sherpa',
    '--mnn-config', (Join-Path $mnnModel 'config.json'), '--mnn-backend', 'cpu', '--mnn-threads', '4',
    '--tts-model', (Join-Path $vitsModel 'model.onnx'),
    '--tts-tokens', (Join-Path $vitsModel 'tokens.txt'),
    '--tts-lexicon', (Join-Path $vitsModel 'lexicon.txt'),
    '--tts-rule-fars', (Join-Path $vitsModel 'rule.far'),
    '--tts-threads', '2', '--strategies', 'P1', '--queue', '4', '--force')
foreach ($approach in @(
        @{ name = 'A'; mode = 'free' },
        @{ name = 'B'; mode = 'template' },
        @{ name = 'C'; mode = 'cache' })) {
    $id = $approach.name
    $jsonl = Join-Path $ResultsRoot "hybrid\$id\results.jsonl"
    $console = Join-Path $ResultsRoot "hybrid\$id\console.out"
    $extra = @('--run-id', $id, '--dialogue-mode', $approach.mode, '--approach', $approach.name,
        '--cases', $casesPath)
    if ($approach.mode -eq 'cache') {
        $extra += @('--bank-manifest', $bankManifest, '--bank-dir', (Join-Path $ResultsRoot 'audio\tts_real'))
    }
    $result = Invoke-Probe -Exe (Join-Path $scenarioStage 'aimj_dialogue_probe.exe') `
        -WorkDir $scenarioStage -ProbeArgs ($hybridBase + $extra) -ConsoleOut $console -Label "hybrid-$($approach.name)"
    Save-Evidence -EvidenceDir (Join-Path $runDir "hybrid-$($approach.name)") -Jsonl $jsonl `
        -ConsoleOut $console -ErrorOut $result.ErrorOut
    $rows = Read-Jsonl $jsonl
    $caseRows = @($rows | Where-Object { $_.type -eq 'case' })
    $bankHits = @($caseRows | Where-Object { $_.cache_hit -eq $true }).Count
    Write-Output ("[hybrid] {0} ({1}): rows={2} bank_hits={3}" -f `
        $approach.name, $approach.mode, $caseRows.Count, $bankHits)
}

# Mock smoke: same harness, mock providers, isolated under smoke/ (never analyzed).
$mockStage = if ($stages.ContainsKey('cpu4')) { $stages['cpu4'] } else { $stages[@($stages.Keys)[0]] }
$mockId = "$RunId-mock-smoke"
$mockConsole = Join-Path $ResultsRoot "smoke\dialogue\$mockId\console.out"
$mockResult = Invoke-Probe -Exe (Join-Path $mockStage 'aimj_dialogue_probe.exe') -WorkDir $mockStage `
    -ProbeArgs @('--cases', $casesPath, '--output', (Join-Path $ResultsRoot 'smoke\dialogue'),
        '--run-id', $mockId, '--llm', 'mock', '--tts', 'mock', '--strategies', 'P0,P1,P2,P3',
        '--rounds', '1', '--queue', '4', '--force') -ConsoleOut $mockConsole -Label 'mock-smoke'
Save-Evidence -EvidenceDir (Join-Path $runDir 'mock-smoke') -Jsonl (Join-Path $ResultsRoot "smoke\dialogue\$mockId\results.jsonl") `
    -ConsoleOut $mockConsole -ErrorOut $mockResult.ErrorOut

# Aggregation: only the four evidence trees are read; smoke/mock stay isolated.
& python -I (Join-Path $repo 'benchmarks\p1_analyze.py') --root $ResultsRoot
if ($LASTEXITCODE -ne 0) { throw 'p1_analyze.py failed' }
$ttsWavDir = Join-Path $ResultsRoot 'audio\tts_real'
if (Test-Path $ttsWavDir) {
    & python -I (Join-Path $repo 'benchmarks\p1_audio_analyze.py') --root $ttsWavDir
    if ($LASTEXITCODE -ne 0) { throw 'p1_audio_analyze.py failed' }
    & python -I (Join-Path $repo 'benchmarks\p1_tts_qc.py') --wav-dir $ttsWavDir `
        --lines $ttsLinesPath --provider sherpa
    if ($LASTEXITCODE -ne 0) { throw 'p1_tts_qc.py failed' }
}

Write-Provenance 'after-runs'
if ($openclNote) { Write-Output "[opencl] $openclNote" }
if ($runHeaders.ContainsKey('opencl')) {
    $openclEvidencePath = Join-Path $runDir 'opencl-evidence-opencl.json'
    $openclVerified = $false
    if (Test-Path $openclEvidencePath) {
        $evidence = Get-Content $openclEvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $openclVerified = $evidence.backend_effective -eq 'opencl_verified' -and
            $evidence.cache_present -eq $true -and [double]$evidence.gpu_util_max_pct -gt 0
    }
    if (-not $openclVerified) {
        Write-Output '[opencl] evidence incomplete: effective/cache/GPU counter missing -> NO-GO per gates'
    } else {
        Write-Output '[opencl] verified: effective=opencl_verified, kernel cache created, GPU utilization > 0'
    }
}
try { Stop-Transcript | Out-Null } catch { }
Write-Output "P1 matrix finished: run_id=$RunId evidence=$runDir"
