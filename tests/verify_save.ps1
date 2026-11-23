<#
.SYNOPSIS
    Verify the C save layer (src/save.c) against the real save files.

.DESCRIPTION
    1. builds bin/test_save.exe (make test_save)
    2. runs the synthetic self-test (50 checks, no game data needed)
    3. parses the 6 real save slots with the thumbnail size taken from
       save.bin's 20-byte header, then re-serialises and requires the output to
       be **byte-identical** to the file on disk
    4. checks ground truth from docs/save-format.md: timestamps, the chapter
       name / current-line strings, thumbnail size, and the save.bin layout
       (1471 globals, 200x113, font name メイリオ)

    Exit codes: 0 = pass, 1 = test failure, 2 = build failure.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_save.ps1

.EXAMPLE
    # a different game / save directory
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_save.ps1 -Game 'D:\games\other'
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$SaveDir = '',
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
if ($SaveDir -eq '') {
    $SaveDir = Join-Path $Game 'save'
}
$savebin = Join-Path $SaveDir 'save.bin'
if (-not (Test-Path $savebin)) {
    Write-Error "save.bin not found: $savebin (pass -SaveDir)"
}

$exe = Join-Path $root 'bin\test_save.exe'
$make = Join-Path $gccbin 'mingw32-make.exe'
if (-not $SkipBuild) {
    Write-Host '== build =='
    Push-Location $root
    try {
        & $make -f Makefile test_save 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            Write-Error "build failed ($LASTEXITCODE)"
            exit 2
        }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path $exe)) {
    Write-Error "missing $exe"
    exit 2
}

$fail = 0

function Check($cond, $msg) {
    if ($cond) {
        Write-Host "  ok    $msg"
    } else {
        Write-Host "  FAIL  $msg" -ForegroundColor Red
        $script:fail++
    }
}

Write-Host '== 1. self-test (synthetic data) =='
$self = & $exe 2>&1 | Out-String
Write-Host $self.Trim()
Check ($LASTEXITCODE -eq 0) 'self-test exit code 0'
Check ($self -match 'test_save: (\d+) passed, 0 failed') 'self-test: 0 failed'
$nself = if ($self -match 'test_save: (\d+) passed, 0 failed') { [int]$Matches[1] } else { 0 }
Check ($nself -ge 50) "self-test ran $nself checks (expected >= 50)"

Write-Host '== 2. real save slots: byte-identical round-trip =='
$slots = @(Get-ChildItem $SaveDir -Filter 's*.bin' |
        Where-Object { $_.Name -match '^s\d{3}\.bin$' } |
        Sort-Object Name | ForEach-Object { $_.FullName })
Check ($slots.Count -ge 1) ("found {0} slot file(s)" -f $slots.Count)
if ($slots.Count -eq 0) {
    Write-Host 'no slots found; nothing to verify' -ForegroundColor Yellow
    exit 1
}

$out = & $exe --savebin $savebin @slots 2>&1 | Out-String
Write-Host $out.Trim()
Check ($LASTEXITCODE -eq 0) 'slot round-trip exit code 0'
Check ($out -match 'test_save: (\d+) passed, 0 failed') 'slot round-trip: 0 failed'

Write-Host '== 3. save.bin layout =='
Check ($out -match 'save\.bin\s+1060635 bytes, 1471 globals, thumb 200x113') `
    'save.bin: 1471 globals, thumb 200x113'
Check ($out -match 'font 8 bytes') 'save.bin: font name is 8 bytes (メイリオ)'

Write-Host '== 4. ground truth from docs/save-format.md =='
foreach ($f in $slots) {
    $name = [System.IO.Path]::GetFileName($f)
    $line = @($out -split "`n" | Where-Object { $_ -match [regex]::Escape($name) })
    if (-not $line) {
        Check $false "$name appears in output"
        continue
    }
    $line = $line[0]
    Check ($line -match 'thumb 200x113') "$name thumbnail is 200x113"
    Check ($line -match ' 202[0-9]-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2} ') `
        "$name has a plausible timestamp"
    if ($name -eq 's481.bin') {
        # 2023-03-19 23:43, 章节名 プロローグ(10B, CP932), 当前行 8B(GBK)
        Check ($line -match '2023-03-19 23:43') 's481.bin timestamp 2023-03-19 23:43'
        Check ($line -match 'str 10/0/8') 's481.bin strings 10/0/8'
    }
    if ($name -eq 's905.bin') {
        Check ($line -match 'str 10/0/14') 's905.bin strings 10/0/14'
    }
}

Write-Host ''
if ($fail -eq 0) {
    Write-Host 'verify_save: PASS' -ForegroundColor Green
    exit 0
}
Write-Host "verify_save: $fail check(s) FAILED" -ForegroundColor Red
exit 1