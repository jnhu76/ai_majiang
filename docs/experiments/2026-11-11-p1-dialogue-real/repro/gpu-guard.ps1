# Samples GPU engine utilization and dedicated GPU memory for one process tree.
# Method validated on this machine (REPORT.md:72 of the P0 experiment): counters
# '\GPU Engine(*)\Utilization Percentage' and '\GPU Process Memory(*)\Dedicated
# Usage', filtered by instance name "pid_<PID>_...".
# Runs until the target process exits, then writes one CSV row per sample.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [int]$IntervalMs = 1000
)
$ErrorActionPreference = 'Stop'
$samples = New-Object System.Collections.Generic.List[object]
while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
    $stamp = (Get-Date).ToString('o')
    $util = 0.0
    $engines = 0
    try {
        $counter = Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop
        $match = @($counter.CounterSamples | Where-Object { $_.InstanceName -like "pid_${ProcessId}_*" })
        if ($match.Count -gt 0) {
            $util = ($match | Measure-Object -Property CookedValue -Sum).Sum
            $engines = $match.Count
        }
    } catch { }
    $memory = [int64]0
    try {
        $counter = Get-Counter '\GPU Process Memory(*)\Dedicated Usage' -ErrorAction Stop
        $match = @($counter.CounterSamples | Where-Object { $_.InstanceName -like "pid_${ProcessId}_*" })
        if ($match.Count -gt 0) {
            $memory = [int64](($match | Measure-Object -Property CookedValue -Sum).Sum)
        }
    } catch { }
    $samples.Add([pscustomobject]@{
        timestamp         = $stamp
        pid               = $ProcessId
        gpu_util_pct      = [math]::Round($util, 3)
        gpu_engine_count  = $engines
        dedicated_bytes   = $memory
    })
    Start-Sleep -Milliseconds $IntervalMs
}
$dir = Split-Path -Parent $OutFile
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$samples | Export-Csv -Path $OutFile -NoTypeInformation -Encoding UTF8
Write-Output ("gpu-guard: {0} samples -> {1}" -f $samples.Count, $OutFile)
