<#
.SYNOPSIS
    Verify the save.bin -> VM globals bridge (src/save_vm.c) end to end.

.DESCRIPTION
    1. builds bin/test_save_vm.exe (make test_save_vm)
    2. runs the synthetic self-test (19 checks: type mapping, int32 wrap,
       capacity limit, stats-only mode, NULL input)
    3. runs against the real save/save.bin and requires the documented type
       census (1471 records = 145 int + 846 unknown + 480 unset) and the first
       values of the int records (5, 5, 1, 0, 0, 180, 255, 128, 100, ...)
    4. drives `fvm.exe --savebin ... --steps 0 --dump-globals ...` and compares
       the dumped globals with that same ground truth (no VM execution, so the
       dump is exactly what the bridge wrote)

    Exit codes: 0 = pass, 1 = test failure, 2 = build failure.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_save_vm.ps1
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Script = '',
    [string]$SaveBin = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path   # fvp\tests
$root = Split-Path -Parent $here                          # fvp
$ws = Split-Path -Parent $root                            # workspace
$gccbin = Join-Path $ws 'tools\w64devkit\w64devkit\bin'
if (Test-Path (Join-Path $gccbin 'gcc.exe')) {
    $env:PATH = "$gccbin;$env:PATH"
}
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Error "gcc not found (expected $gccbin)"
}
if ($SaveBin -eq '') { $SaveBin = Join-Path $Game 'save\save.bin' }
if ($Script -eq '') { $Script = Join-Path $Game 'iroseka.hcb' }
if (-not (Test-Path $SaveBin)) { Write-Error "save.bin not found: $SaveBin"; exit 2 }
if (-not (Test-Path $Script)) { Write-Error "script not found: $Script"; exit 2 }

$exe = Join-Path $root 'bin\test_save_vm.exe'
$fvm = Join-Path $root 'bin\fvm.exe'
$make = Join-Path $gccbin 'mingw32-make.exe'
if (-not $SkipBuild) {
    Write-Host '== build =='
    Push-Location $root
    try {
        & $make -f Makefile test_save_vm all 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { Write-Error "build failed ($LASTEXITCODE)"; exit 2 }
    } finally { Pop-Location }
}
foreach ($p in @($exe, $fvm)) {
    if (-not (Test-Path $p)) { Write-Error "missing $p"; exit 2 }
}

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

Write-Host '== 1. self-test (synthetic records) =='
$self = & $exe 2>&1 | Out-String
Write-Host $self.Trim()
Check ($LASTEXITCODE -eq 0) 'self-test exit code 0'
Check ($self -match 'test_save_vm: (\d+) passed, 0 failed') 'self-test: 0 failed'
$nself = if ($self -match 'test_save_vm: (\d+) passed, 0 failed') { [int]$Matches[1] } else { 0 }
Check ($nself -ge 19) "self-test ran $nself checks (expected >= 19)"

Write-Host '== 2. real save.bin: documented type census =='
$real = & $exe $SaveBin 2>&1 | Out-String
Write-Host $real.Trim()
Check ($LASTEXITCODE -eq 0) 'real-file exit code 0'
Check ($real -match 'test_save_vm: (\d+) passed, 0 failed') 'real-file: 0 failed'
Check ($real -match 'records=1471 int=145 unknown=846 unset=480') `
    'census: 1471 = 145 int + 846 unknown(type1) + 480 unset'

Write-Host '== 3. fvm --savebin --dump-globals (no VM execution) =='
$outDir = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$dump = Join-Path $outDir 'globals-seed.txt'
$run = & $fvm $Script --savebin $SaveBin --steps 0 --quiet --dump-globals $dump 2>&1 |
        Out-String
Write-Host ($run.Trim() -split "`n" | Select-Object -First 1)
Check ($LASTEXITCODE -eq 0) 'fvm exit code 0'
Check ($run -match 'save\.bin: 1471 records, 145 int applied, 846 unknown') `
    'fvm reports the same census'
Check (Test-Path $dump) 'dump file written'

$lines = @(Get-Content $dump | Where-Object { $_ -match '^\d+ -?\d+$' })
Check ($lines.Count -eq 114) "dump has 114 non-zero int globals (got $($lines.Count))"

# 文档 §3.3 的值序列：5,5,1,0,0,180,... —— 0 被 dump 跳过，所以期望：
$expect = @('0 5', '1 5', '2 1', '6 180', '7 255', '8 128', '9 100', '10 90',
            '11 70', '12 70', '13 65', '14 100')
for ($i = 0; $i -lt $expect.Count; $i++) {
    $got = if ($i -lt $lines.Count) { $lines[$i].Trim() } else { '<missing>' }
    Check ($got -eq $expect[$i]) "seed[$i] = '$($expect[$i])' (got '$got')"
}

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_save_vm: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_save_vm: $fail check(s) FAILED" -ForegroundColor Red
exit 1