#Requires -Version 5.1
<#
.SYNOPSIS
    Memory-growth curve: is the desktop shell caching assets or leaking?

.DESCRIPTION
    `verify_soak.ps1` only puts a ceiling on growth. This script records the whole curve
    (working set every N seconds for M seconds) and answers the real question:
    does growth CONVERGE (texture/asset caches filling up) or keep climbing linearly (leak)?

    It reports:
      - the full (t, MiB) series
      - growth over the first half vs the last quarter
      - a verdict: CONVERGED (last-quarter growth <= threshold) or STILL-GROWING

    Exit codes: 0 = converged, 1 = still growing (or the shell misbehaved), 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_memory.ps1
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_memory.ps1 -Seconds 600 -Every 30
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [int]$Seconds = 480,
    [int]$Every = 30,
    [double]$ConvergedGrowthMiB = 8,
    [string]$Csv = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$exe = Join-Path $root 'bin\fvp.exe'
if (-not (Test-Path $exe)) { Write-Error "missing $exe"; exit 2 }
if ($Csv -eq '') { $Csv = Join-Path $root 'out\memory-curve.csv' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Csv) | Out-Null

Write-Host "== memory curve: $Seconds s, sample every $Every s =="
$proc = Start-Process -FilePath $exe -ArgumentList @('--game', $Game,
    '--run-seconds', $Seconds) -WorkingDirectory $root -PassThru

$samples = New-Object System.Collections.Generic.List[object]
$t0 = Get-Date
$deadline = $t0.AddSeconds($Seconds + 90)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $Every
    try {
        $p = Get-Process -Id $proc.Id -ErrorAction Stop
        $mb = [math]::Round($p.WorkingSet64 / 1MB, 1)
        $t = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
        $samples.Add([pscustomobject]@{ t = $t; MiB = $mb })
        Write-Host ("  t={0,6:N1}s  {1,7:N1} MiB" -f $t, $mb)
    } catch { break }
}

$exitOk = $false
if (-not $proc.HasExited) {
    Write-Host '  shell did not exit in time; killing' -ForegroundColor Yellow
    Stop-Process -Id $proc.Id -Force
} else {
    $exitOk = ($proc.ExitCode -eq 0)
}

$rows = @($samples | ForEach-Object { "{0},{1}" -f $_.t, $_.MiB })
[System.IO.File]::WriteAllLines($Csv, (@('t_seconds,MiB') + $rows))

if ($samples.Count -lt 3) {
    Write-Host '  not enough samples' -ForegroundColor Red
    exit 1
}

$first = $samples[0].MiB
$mid = $samples[[int]($samples.Count / 2)].MiB
$last = $samples[$samples.Count - 1].MiB
$quarterStart = $samples[[int]($samples.Count * 0.75)].MiB
$lastQuarter = $last - $quarterStart

Write-Host ''
Write-Host ("  first {0:N1} -> mid {1:N1} -> last {2:N1} MiB" -f $first, $mid, $last)
Write-Host ("  first half growth {0:N1} MiB, last quarter growth {1:N1} MiB" -f ($mid - $first), $lastQuarter)
Write-Host ("  csv -> {0}" -f $Csv)

$fail = 0
if (-not $exitOk) { Write-Host '  FAIL shell exit code' -ForegroundColor Red; $fail++ }
if ($lastQuarter -le $ConvergedGrowthMiB) {
    Write-Host ("  VERDICT: CONVERGED (last quarter +{0:N1} MiB <= {1} MiB)" -f $lastQuarter, $ConvergedGrowthMiB) -ForegroundColor Green
} else {
    Write-Host ("  VERDICT: STILL GROWING (last quarter +{0:N1} MiB > {1} MiB) -> treat as leak until proven otherwise" -f $lastQuarter, $ConvergedGrowthMiB) -ForegroundColor Red
    $fail++
}

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_memory: PASS (converged)' -ForegroundColor Green; exit 0 }
Write-Host 'verify_memory: FAIL' -ForegroundColor Red
exit 1