#Requires -Version 5.1
<#
.SYNOPSIS
    Soak test: run the desktop shell for a while and assert it stays healthy.

.DESCRIPTION
    Long runs are where leaks, audio underruns and frame drops show up. This script
    launches bin/fvp.exe with --run-seconds, samples its working set while it runs, and
    then checks the log:

      1. the process exits by itself with code 0 (no hang, no crash)
      2. working set growth stays under a threshold (default 96 MiB)
      3. every `perf:` line after the first reports >= 55 fps and `slow=` at most 2
      4. every `audio:` line reports `underruns=0`
      5. the script progressed: text lines > 0 and VM steps > 0
      6. no `error`/`fatal`/`cannot` lines in the log

    Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_soak.ps1
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_soak.ps1 -Seconds 1800
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [int]$Seconds = 180,
    [double]$MaxGrowthMiB = 96,
    [double]$MinFps = 55,
    [switch]$WithMotion
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$exe = Join-Path $root 'bin\fvp.exe'
$log = Join-Path $root 'bin\fvp-win32.log'
foreach ($p in @($exe, (Join-Path $Game 'iroseka.hcb'))) {
    if (-not (Test-Path $p)) { Write-Error "missing $p"; exit 2 }
}

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

Write-Host "== soak: $Seconds s =="
$args = @('--game', $Game, '--run-seconds', $Seconds, '--motion', '--save-preview-every', '0')
if (-not $WithMotion) { $args = @('--game', $Game, '--run-seconds', $Seconds) }
$proc = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $root -PassThru

$minWs = [double]::MaxValue
$maxWs = 0.0
$deadline = (Get-Date).AddSeconds($Seconds + 90)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    try {
        $p = Get-Process -Id $proc.Id -ErrorAction Stop
        $mb = $p.WorkingSet64 / 1MB
        if ($mb -lt $minWs) { $minWs = $mb }
        if ($mb -gt $maxWs) { $maxWs = $mb }
    } catch { break }
}
if (-not $proc.HasExited) {
    Write-Host '  shell did not exit in time; killing' -ForegroundColor Yellow
    Stop-Process -Id $proc.Id -Force
    $fail++
} else {
    Check ($proc.ExitCode -eq 0) "shell exit code $($proc.ExitCode)"
}

$growth = $maxWs - $minWs
Write-Host ("  working set {0:N1} .. {1:N1} MiB (growth {2:N1} MiB)" -f $minWs, $maxWs, $growth)
Check ($minWs -ne [double]::MaxValue) 'working set was sampled'
Check ($growth -le $MaxGrowthMiB) ("growth {0:N1} MiB <= {1} MiB" -f $growth, $MaxGrowthMiB)

$text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
function LinesLike([string]$pattern) {
    return @([regex]::Matches($text, $pattern) | ForEach-Object { $_.Value })
}

$perfLines = @($text -split "`n" | Where-Object { $_ -match 'perf: fps=' })
Check ($perfLines.Count -ge 2) "log has $($perfLines.Count) perf line(s)"
$badFps = @($perfLines | Where-Object {
    if ($_ -match 'fps=([0-9.]+)') { [double]$Matches[1] -lt $MinFps } else { $false }
})
$badSlow = @($perfLines | Where-Object {
    if ($_ -match 'slow=(\d+)') { [int]$Matches[1] -gt 2 } else { $false }
})
Check ($badFps.Count -eq 0) ("all perf lines >= $MinFps fps ({0} bad)" -f $badFps.Count)
Check ($badSlow.Count -eq 0) ("slow frames <= 2 on every 5 s window ({0} bad)" -f $badSlow.Count)

$audioLines = @($text -split "`n" | Where-Object { $_ -match 'audio: .*submitted=' })
$underrun = @($audioLines | Where-Object { $_ -match 'underruns=(\d+)' -and [int]$Matches[1] -gt 0 })
Check ($audioLines.Count -gt 0) "log has $($audioLines.Count) audio line(s)"
Check ($underrun.Count -eq 0) ("0 audio underruns across {0} window(s)" -f $audioLines.Count)

$steps = @($text -split "`n" | Where-Object { $_ -match 'vm:\s+(\d+) steps' })
Check ($steps.Count -gt 0) 'VM kept stepping'
$lastText = @($text -split "`n" | Where-Object { $_ -match 'text lines (\d+)' } | Select-Object -Last 1)
if ($lastText.Count -gt 0 -and $lastText[0] -match 'text lines (\d+)') {
    Check ([int]$Matches[1] -gt 0) "script produced $($Matches[1]) text line(s)"
} else {
    Check $false 'text line counter found in the log'
}

$bad = @($text -split "`n" | Where-Object { $_ -match 'error|fatal|cannot |crash' })
Check ($bad.Count -eq 0) ("no error lines ({0} found)" -f $bad.Count)
if ($bad.Count -gt 0) { $bad | Select-Object -First 5 | ForEach-Object { Write-Host "      $_" } }

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_soak: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_soak: $fail check(s) FAILED" -ForegroundColor Red
exit 1