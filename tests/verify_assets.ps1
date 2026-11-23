# verify_assets.ps1 -- build the C asset layer and run the full acceptance.
#
#   powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1
#   powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1 -Quick
#   powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1 -Game "D:\game"
#
# Steps:
#   1. compile c_assets.exe with the bundled w64devkit gcc (C99, no zlib)
#   2. run tests/verify_assets.py, which compares everything against the
#      Python reference (zlib.decompress / work/hzc1.py) and prints PASS/FAIL
param(
    [switch]$Quick,
    [switch]$Full,
    [string]$Game = '',
    [string]$Extract = '',
    [int]$Seed = 0,
    [switch]$NoBuild,
    [Parameter(ValueFromRemainingArguments = $true)]$Rest
)
$ErrorActionPreference = 'Stop'

$Tests = Split-Path -Parent $MyInvocation.MyCommand.Path
$Fvp = Split-Path -Parent $Tests
$Workspace = Split-Path -Parent $Fvp
$DevkitBin = Join-Path $Workspace 'tools\w64devkit\w64devkit\bin'
$Gcc = Join-Path $DevkitBin 'gcc.exe'

$BinDir = Join-Path $Fvp 'bin'
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
$Exe = Join-Path $BinDir 'c_assets.exe'

if (-not $NoBuild) {
    if (-not (Test-Path $Gcc)) {
        throw "gcc not found at $Gcc"
    }
    $env:PATH = "$DevkitBin;$env:PATH"
    $Sources = @(
        (Join-Path $Tests 'c_assets.c'),
        (Join-Path $Fvp 'src\bin.c'),
        (Join-Path $Fvp 'src\inflate.c'),
        (Join-Path $Fvp 'src\hzc1.c')
    )
    Write-Host '== build =='
    & $Gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv `
        -I (Join-Path $Fvp 'src') -o $Exe @Sources
    if ($LASTEXITCODE -ne 0) {
        throw "build failed with exit code $LASTEXITCODE"
    }
    Write-Host "built: $Exe"
}

$Python = $env:FVP_PYTHON
if (-not $Python) {
    $Python = 'C:\Users\Mechrevo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
}
if (-not (Test-Path $Python)) {
    $Python = 'python'
}
Write-Host "python: $Python"
Write-Host ''

$PyArgs = @((Join-Path $Tests 'verify_assets.py'), '--exe', $Exe)
if ($Quick) { $PyArgs += '--quick' }
if ($Full) { $PyArgs += '--full' }
if ($Game) { $PyArgs += @('--game', $Game) }
if ($Extract) { $PyArgs += @('--extract', $Extract) }
if ($Seed -ne 0) { $PyArgs += @('--seed', "$Seed") }
if ($Rest) { $PyArgs += $Rest }

& $Python @PyArgs
$Code = $LASTEXITCODE

Write-Host ''
if ($Code -eq 0) {
    Write-Host 'verify_assets: PASS'
} else {
    Write-Host "verify_assets: FAIL (exit code $Code)"
}
exit $Code