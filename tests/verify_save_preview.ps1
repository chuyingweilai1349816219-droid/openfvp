#Requires -Version 5.1
<#
.SYNOPSIS
    Verify the shell's save-preview path end to end (write real slots from a live run).

.DESCRIPTION
    1. launches bin/fvp.exe with --run-seconds / --save-template / --save-preview[-every]
    2. waits for it to exit (it writes one preview per period plus one at exit)
    3. reads every produced slot back with bin/test_save.exe and requires 0 failures,
       and requires the engine-state tail to stay at the template's length

    Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_save_preview.ps1
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Template = '',
    [int]$Seconds = 6,
    [int]$Every = 2,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$ws = Split-Path -Parent $root
if ($Template -eq '') { $Template = Join-Path $Game 'save\s485.bin' }
if ($OutDir -eq '') { $OutDir = Join-Path $root 'out' }

$exe = Join-Path $root 'bin\fvp.exe'
$tester = Join-Path $root 'bin\test_save.exe'
$savebin = Join-Path $Game 'save\save.bin'
foreach ($p in @($exe, $tester, $Template, $savebin)) {
    if (-not (Test-Path $p)) { Write-Error "missing $p"; exit 2 }
}

$fail = 0
function Check($cond, $msg) {
    if ($cond) { Write-Host "  ok    $msg" }
    else { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
}

$preview = Join-Path $OutDir 'verify-preview.bin'
Get-ChildItem (Join-Path $OutDir 'verify-preview.bin*') -ErrorAction SilentlyContinue |
    Remove-Item -Force

Write-Host "== run: $Seconds s, preview every $Every s =="
$log = Join-Path $root 'bin\fvp-win32.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$argList = @('--game', $Game, '--run-seconds', $Seconds,
             '--save-template', $Template, '--save-preview', $preview,
             '--save-preview-every', $Every)
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $root -PassThru
$deadline = (Get-Date).AddSeconds($Seconds + 40)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
}
if (-not $proc.HasExited) {
    Write-Host '  shell did not exit in time; killing' -ForegroundColor Yellow
    Stop-Process -Id $proc.Id -Force
    $fail++
} else {
    Check ($proc.ExitCode -eq 0) "shell exit code $($proc.ExitCode)"
}

$logText = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
$writes = ([regex]::Matches($logText, 'save-preview: wrote')).Count
Check ($writes -ge 2) "log shows $writes preview write(s) (expected >= 2)"
Check ($logText -match 'save-preview: thumbnail replaced \(200x113\)') `
    'thumbnail was replaced from the live canvas'

$files = @(Get-ChildItem (Join-Path $OutDir 'verify-preview.bin*') -ErrorAction SilentlyContinue |
           Sort-Object Name | ForEach-Object { $_.FullName })
Check ($files.Count -ge 2) "produced $($files.Count) slot file(s)"
if ($files.Count -gt 0) {
    $out = & $tester --savebin $savebin @files 2>&1 | Out-String
    Write-Host ($out.Trim())
    Check ($LASTEXITCODE -eq 0) 'test_save exit code 0'
    Check ($out -match 'test_save: (\d+) passed, 0 failed') 'every slot reads back with 0 failures'
    Check ($out -match 'str 10/0/') 'the script-provided strings reached the slots'
    $tail = '598950'
    Check (([regex]::Matches($out, "tail $tail")).Count -eq $files.Count) `
        "engine-state tail stayed at the template's $tail bytes in all slots"
}

Get-ChildItem (Join-Path $OutDir 'verify-preview.bin*') -ErrorAction SilentlyContinue |
    Remove-Item -Force

Write-Host ''
if ($fail -eq 0) { Write-Host 'verify_save_preview: PASS' -ForegroundColor Green; exit 0 }
Write-Host "verify_save_preview: $fail check(s) FAILED" -ForegroundColor Red
exit 1