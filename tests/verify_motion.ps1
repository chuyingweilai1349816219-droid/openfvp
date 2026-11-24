#Requires -Version 5.1
<#
.SYNOPSIS
    Deterministic A/B for the animation layer: motion OFF vs ON at fixed VM steps.

.DESCRIPTION
    The desktop shell is wall-clock driven, so two "15 second" runs do NOT reach the
    same script position (measured: two default runs already differ).  This script
    uses tests/render_vm.c instead — a fixed number of VM steps per frame and a fixed
    animation dt — which makes the comparison meaningful.

    Assertions:
      1. motion OFF twice at the same step count -> identical PNG (harness is deterministic)
      2. at 1,000 steps  (t ~ 0)  OFF == ON  (enabling animation must not shift a fresh frame)
      3. at 60,000 steps          OFF != ON  (animation really changes pixels)

    Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_motion.ps1
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Script = '',
    [int]$Steps = 60000,
    [int]$EarlySteps = 1000,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$ws = Split-Path -Parent $root
$gccbin = Join-Path $ws 'tools\w64devkit\w64devkit\bin'
if (Test-Path (Join-Path $gccbin 'gcc.exe')) { $env:PATH = "$gccbin;$env:PATH" }
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Error "gcc not found (expected $gccbin)"; exit 2
}
if ($Script -eq '') { $Script = Join-Path $Game 'iroseka.hcb' }
if (-not (Test-Path $Script)) { Write-Error "script not found: $Script"; exit 2 }

$exe = Join-Path $root 'bin\render_vm.exe'
if (-not $SkipBuild) {
    Write-Host '== build render_vm (with motion) =='
    Push-Location $root
    try {
        $srcs = @('src\fvp.c', 'src\encoding.c', 'src\hcb.c', 'src\vm.c', 'src\syscall.c',
                  'src\flags.c', 'src\textattr.c', 'src\gfx.c', 'src\motion.c',
                  'src\timer.c', 'src\history.c',
                  'src\bin.c', 'src\hzc1.c', 'src\inflate.c')
        & gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv -Isrc -Isrc/platform/win32 `
            -o 'bin\render_vm.exe' 'tests\render_vm.c' @srcs -lm
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed ($LASTEXITCODE)"; exit 2 }
    } finally { Pop-Location }
}
if (-not (Test-Path $exe)) { Write-Error "missing $exe"; exit 2 }

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

function Render([string]$outDir, [int]$steps, [switch]$motion) {
    if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $a = @($Script, '--game', $Game, '--steps', $steps, '--out', $outDir, '--quiet')
    if ($motion) { $a += '--motion' }
    Push-Location $root
    try {
        & $exe @a 2>&1 | Out-Null
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $png = Join-Path $outDir 'vm-frame.png'
    if ($code -ne 0 -or -not (Test-Path $png)) { return $null }
    return (Get-FileHash $png -Algorithm SHA256).Hash
}

$offA = Render (Join-Path $root 'out\motion-off-a') $Steps
$offB = Render (Join-Path $root 'out\motion-off-b') $Steps
$on = Render (Join-Path $root 'out\motion-on') $Steps -motion

$offEarly = Render (Join-Path $root 'out\motion-off-early') $EarlySteps
$onEarly = Render (Join-Path $root 'out\motion-on-early') $EarlySteps -motion

Write-Host "== results ($Steps steps unless noted) =="
Write-Host ("  off A  {0}" -f $offA)
Write-Host ("  off B  {0}" -f $offB)
Write-Host ("  on     {0}" -f $on)
Write-Host ("  off@$EarlySteps {0}" -f $offEarly)
Write-Host ("  on @$EarlySteps {0}" -f $onEarly)

Check ($offA -ne $null -and $offB -ne $null) 'both motion-off runs produced a frame'
Check ($offA -eq $offB) 'harness is deterministic (off A == off B)'
Check ($onEarly -ne $null -and $offEarly -ne $null) 'early frames produced'
Check ($onEarly -eq $offEarly) "t~0 frame is identical with motion on/off ($EarlySteps steps)"
Check ($on -ne $offA) "motion on/off differ at $Steps steps (animation is visible)"

foreach ($d in @('motion-off-a', 'motion-off-b', 'motion-on', 'motion-off-early',
                 'motion-on-early')) {
    $p = Join-Path $root "out\$d"
    if (Test-Path $p) { Remove-Item $p -Recurse -Force }
}

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_motion: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_motion: $fail check(s) FAILED" -ForegroundColor Red
exit 1