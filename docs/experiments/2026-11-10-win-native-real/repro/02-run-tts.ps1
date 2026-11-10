# Stage 2: real Chinese TTS with sherpa-onnx v2.0.1 prebuilt win-x64 binaries.
# Works from any working directory; paths are derived from this script's location.
# HTTP(S) proxy comes from ambient environment variables; none is hardcoded here.
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$extern = Join-Path (Split-Path -Parent $repo) 'ai_majiang-extern'
$s      = Join-Path $extern 'third_party\sherpa-onnx-extract\sherpa-onnx-v2.0.1-win-x64-shared-MD-Release'
$m      = Join-Path $extern 'third_party\models\vits-icefall-zh-aishell3'
$ttsDir = Join-Path $repo 'benchmarks\results\tts'
Write-Output "repo=$repo"
Write-Output "extern=$extern"
foreach ($p in (Join-Path $s 'bin\sherpa-onnx-offline-tts.exe'), (Join-Path $m 'model.onnx'), (Join-Path $m 'rule.far')) {
    if (-not (Test-Path $p)) { throw "missing asset: $p - run 00-setup-extern.ps1 first" }
}
New-Item -ItemType Directory -Force $ttsDir | Out-Null

# 3 speakers x 3 mahjong lines (short / normal / long). sid indexes speakers.txt (174 speakers).
$lines = @('碰！', '别磨蹭，快出牌！', '你这牌打得肥嘟嘟的，我都等了三轮了，快点出牌吧。')
foreach ($sid in 0,1,100) {
    for ($i = 0; $i -lt 3; $i++) {
        $wav = Join-Path $ttsDir ("tts_sid{0}_line{1}.wav" -f $sid, ($i+1))
        & (Join-Path $s 'bin\sherpa-onnx-offline-tts.exe') `
            --vits-model="$m\model.onnx" --vits-lexicon="$m\lexicon.txt" --vits-tokens="$m\tokens.txt" `
            --tts-rule-fars="$m\rule.far" --num-threads=2 --sid=$sid `
            --output-filename="$wav" $lines[$i] | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "tts synthesis failed (exit $LASTEXITCODE) for sid=$sid line=$($i+1)" }
    }
}
# Every output must be a non-empty file; sizes are printed for the record.
$failed = $false
Get-ChildItem $ttsDir -Filter 'tts_sid*.wav' | ForEach-Object {
    Write-Output ("{0}  {1} bytes" -f $_.Name, $_.Length)
    if ($_.Length -eq 0) { $failed = $true }
}
if ($failed) { throw "empty WAV produced" }
if ((Get-ChildItem $ttsDir -Filter 'tts_sid*.wav').Count -ne 9) { throw "expected 9 WAV files" }
Write-Output "TTS smoke OK: 9 non-empty WAVs in $ttsDir"
