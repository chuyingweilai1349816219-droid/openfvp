#Requires -Version 5.1
<#
.SYNOPSIS
    Check the virtual clock (--timeline) without disturbing the byte-exact baseline.

.DESCRIPTION
    The time model is opt-in. This script asserts the three things that matter today:

      1. legacy mode (default) reports `virtual ms 0` and still matches the frozen
         byte-exact text baseline (the same gate as compare.py --run);
      2. `--timeline` finishes the same step budget without hanging and advances the
         virtual clock (`virtual ms > 0`);
      3. at 900k steps the timeline run produces the SAME text as legacy — i.e. the new
         scheduler is currently behaviour-neutral on this title. If a future change makes
         it diverge, this script fails loudly instead of quietly rewriting the baseline.

    Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_timeline.ps1
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Script = '',
    [int]$Steps = 900000
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Script -eq '') { $Script = Join-Path $Game 'iroseka.hcb' }
$fvm = Join-Path $root 'bin\fvm.exe'
$ref = Join-Path $root 'ref\ref-900k.txt'
foreach ($p in @($fvm, $Script, $ref)) {
    if (-not (Test-Path $p)) { Write-Error "missing $p"; exit 2 }
}

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

$outDir = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$offTxt = Join-Path $outDir 'timeline-off.txt'
$onTxt = Join-Path $outDir 'timeline-on.txt'

Write-Host "== legacy (default) @ $Steps steps =="
$off = & $fvm $Script --steps $Steps --click 600 --quiet --text-out $offTxt 2>&1 | Out-String
Write-Host ($off.Trim())
Check ($LASTEXITCODE -eq 0) 'legacy run exit 0'
Check ($off -match 'virtual ms 0') 'legacy keeps the clock at 0'

Write-Host "== --timeline @ $Steps steps =="
$on = & $fvm $Script --steps $Steps --click 600 --quiet --timeline --text-out $onTxt 2>&1 | Out-String
Write-Host ($on.Trim())
Check ($LASTEXITCODE -eq 0) 'timeline run exit 0 (no hang)'
$ms = if ($on -match 'virtual ms (\d+)') { [int]$Matches[1] } else { -1 }
Check ($ms -gt 0) "virtual clock advanced ($ms ms)"

$offBytes = if (Test-Path $offTxt) { [System.IO.File]::ReadAllBytes($offTxt) } else { @() }
$onBytes = if (Test-Path $onTxt) { [System.IO.File]::ReadAllBytes($onTxt) } else { @() }
Check ($offBytes.Length -eq $onBytes.Length) ("same text size ({0} vs {1} bytes)" -f $offBytes.Length, $onBytes.Length)
$same = $false
if ($offBytes.Length -eq $onBytes.Length -and $offBytes.Length -gt 0) {
    $same = $true
    for ($i = 0; $i -lt $offBytes.Length; $i++) {
        if ($offBytes[$i] -ne $onBytes[$i]) { $same = $false; break }
    }
}
Check $same 'timeline text is byte-identical to legacy at this step budget'

# frozen baseline still matches (the real gate lives in compare.py, repeated here so
# a timeline regression cannot slip through when only this script is run)
$refBytes = [System.IO.File]::ReadAllBytes($ref)
Check ($refBytes.Length -eq $offBytes.Length) 'legacy text size matches ref-900k.txt'
$refSame = $refBytes.Length -eq $offBytes.Length
if ($refSame) {
    for ($i = 0; $i -lt $refBytes.Length; $i++) {
        if ($refBytes[$i] -ne $offBytes[$i]) { $refSame = $false; break }
    }
}
Check $refSame 'legacy text is byte-identical to ref-900k.txt'

Remove-Item $offTxt, $onTxt -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_timeline: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_timeline: $fail check(s) FAILED" -ForegroundColor Red
exit 1