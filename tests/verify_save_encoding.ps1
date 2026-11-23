#Requires -Version 5.1
<#
.SYNOPSIS
    Verify --save-encoding: the same script strings must land as CP932 or GBK bytes.

.DESCRIPTION
    Runs the shell twice against the real game (once with the default CP932 save
    encoding, once with --save-encoding gbk), then dumps the produced slots'
    strings with `test_save.exe --dump-strs` and compares them with the byte
    sequences Python's codecs produce for `プロローグ`:

        cp932: 83 76 83 8d 83 8d 81 5b 83 4f
        gbk  : a5 d7 a5 ed a5 ed a9 60 a5 b0

    Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_save_encoding.ps1
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Template = '',
    [int]$Seconds = 6,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Template -eq '') { $Template = Join-Path $Game 'save\s485.bin' }
if ($OutDir -eq '') { $OutDir = Join-Path $root 'out' }

$exe = Join-Path $root 'bin\fvp.exe'
$tester = Join-Path $root 'bin\test_save.exe'
foreach ($p in @($exe, $tester, $Template)) {
    if (-not (Test-Path $p)) { Write-Error "missing $p"; exit 2 }
}

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

function Run-Preview([string]$path, [string]$encoding) {
    $args = @('--game', $Game, '--run-seconds', $Seconds,
              '--save-template', $Template, '--save-preview', $path)
    if ($encoding -ne '') { $args += @('--save-encoding', $encoding) }
    $proc = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $root -PassThru
    $deadline = (Get-Date).AddSeconds($Seconds + 40)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        return $false
    }
    return ($proc.ExitCode -eq 0) -and (Test-Path $path)
}

$cp932 = Join-Path $OutDir 'enc-check-cp932.bin'
$gbk = Join-Path $OutDir 'enc-check-gbk.bin'
foreach ($p in @($cp932, $gbk)) { Remove-Item $p -Force -ErrorAction SilentlyContinue }

Write-Host '== run 1: default (CP932) =='
Check (Run-Preview $cp932 '') 'shell wrote the CP932 preview slot'
Write-Host '== run 2: --save-encoding gbk =='
Check (Run-Preview $gbk 'gbk') 'shell wrote the GBK preview slot'

if ($fail -eq 0) {
    $out = & $tester --dump-strs $cp932 $gbk 2>&1 | Out-String
    Write-Host $out.Trim()
    Check ($LASTEXITCODE -eq 0) 'test_save --dump-strs exit code 0'

    # 每个文件的前 4 行：文件名行 + str0/str1/str2
    $lines = @($out -split "`n" | Where-Object { $_ -match '^\s+str\d=' })
    Check ($lines.Count -ge 6) "dumped $($lines.Count) string lines (expected 6)"

    $cp932Str0 = ($lines[0] -replace '\s+', ' ').Trim()
    $gbkStr0 = ($lines[3] -replace '\s+', ' ').Trim()
    Check ($cp932Str0 -eq 'str0=10: 83 76 83 8d 83 8d 81 5b 83 4f') `
        "CP932 str0 is プロローグ (got '$cp932Str0')"
    Check ($gbkStr0 -eq 'str0=10: a5 d7 a5 ed a5 ed a9 60 a5 b0') `
        "GBK str0 is プロローグ (got '$gbkStr0')"
    Check ($cp932Str0 -ne $gbkStr0) 'the two encodings really differ'
}

foreach ($p in @($cp932, $gbk)) { Remove-Item $p -Force -ErrorAction SilentlyContinue }

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_save_encoding: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_save_encoding: $fail check(s) FAILED" -ForegroundColor Red
exit 1