<#
.SYNOPSIS
    End-to-end verification: the script VM really plays audio through src/snd.c.

.DESCRIPTION
    1. builds bin\fvm.exe / bin\libfvp.a (make all lib) and
       bin\audio_vm.exe (direct gcc, the Makefile is intentionally not
       changed) from the current sources;
    2. bin\audio_vm.exe runs iroseka.hcb for -Steps with the audio context
       attached (fvp_vm_attach_snd), every 6 audio BIN packages registered as
       the resource bank, mixes 1024 frames per 1000 VM steps and reports the
       played resources / resolve+decode failures / peak / RMS; it also dumps
       out\c\audio-900k.wav when -WavOut is set;
    3. the two byte-exact acceptance cases are re-run (tests\compare.py,
       900k and 2M steps) to prove bin\fvm.exe still behaves exactly as
       before (fvm.exe never attaches the audio context);
    4. the snd.c resampling guard is checked against the fuzz repro
       out\fuzz\repro\wav_rate1_amplification.wav: it must be rejected
       without a 168 MB allocation (peak allocation stays tiny).

    Exit codes: 0 = pass, 1 = build/setup failure, 2 = audio run failed,
                3 = byte-exact compare failed, 4 = resample guard failed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_audio.ps1

.EXAMPLE
    # reuse already built binaries, skip the byte compares and the repro
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_audio.ps1 -SkipBuild -SkipByteCompare -SkipRepro
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Script = '',
    [int]$Steps = 900000,
    [int]$Click = 600,
    [string]$Out = 'out\c',
    [string]$Python = 'C:\Users\Mechrevo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe',
    [switch]$SkipBuild,
    [switch]$SkipByteCompare,
    [switch]$SkipRepro,
    [switch]$WavOut
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
    exit 1
}
if (-not (Test-Path $Python)) {
    Write-Error "python not found: $Python"
    exit 1
}
if (-not (Test-Path $Game)) {
    Write-Error "game directory not found: $Game (pass -Game <dir>)"
    exit 1
}
if ([string]::IsNullOrEmpty($Script)) {
    $Script = Join-Path $Game 'iroseka.hcb'
}
if (-not (Test-Path $Script)) {
    Write-Error "script not found: $Script"
    exit 1
}

$audioExe = Join-Path $root 'bin\audio_vm.exe'
$wav = Join-Path $root (Join-Path $Out 'audio-900k.wav')
$repro = Join-Path $root 'out\fuzz\repro\wav_rate1_amplification.wav'
$fuzzExe = Join-Path $root 'out\fuzz\fuzz_snd_rate1.exe'

Push-Location $root
$code = 0
try {
    # native tools write progress to stderr; do not turn that into a crash
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    New-Item -ItemType Directory -Force -Path $Out | Out-Null

    if (-not $SkipBuild) {
        Write-Host '[1/4] building bin\fvm.exe / bin\libfvp.a ...'
        & make all lib
        if ($LASTEXITCODE -ne 0) { throw 'make all lib failed' }

        Write-Host '[2/4] building bin\audio_vm.exe (direct gcc, no Makefile change) ...'
        & gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv -Isrc `
            -o $audioExe tests\audio_vm.c `
            src\fvp.c src\encoding.c src\hcb.c src\vm.c src\syscall.c src\flags.c `
            src\snd.c src\bin.c`
            src\textattr.c src\save.c src\save_vm.c src\thumb.c src\timer.c src\history.c -lm
        if ($LASTEXITCODE -ne 0) { throw 'building audio_vm.exe failed' }
    }
    if (-not (Test-Path $audioExe)) {
        throw "missing $audioExe (build without -SkipBuild)"
    }

    Write-Host ('[3/4] running the VM + audio layer for {0} steps ...' -f $Steps)
    $audioArgs = @($Script, '--game', $Game, '--steps', "$Steps", '--click', "$Click",
                   '--mix-frames', '1024', '--chunk', '1000')
    if ($WavOut) { $audioArgs += @('--wav-out', $wav) }
    & $audioExe @audioArgs
    if ($LASTEXITCODE -ne 0) { throw "audio_vm.exe failed (exit $LASTEXITCODE)" }

    if (-not $SkipByteCompare) {
        Write-Host '[4/4] byte-exact acceptance: compare.py (900k) ...'
        & $Python 'tests\compare.py' --run
        if ($LASTEXITCODE -ne 0) {
            $code = 3
            throw 'compare.py --run failed'
        }
        Write-Host '[4/4] byte-exact acceptance: compare.py (2M) ...'
        & $Python 'tests\compare.py' --run --steps 2000000
        if ($LASTEXITCODE -ne 0) {
            $code = 3
            throw 'compare.py --run --steps 2000000 failed'
        }
    }

    if (-not $SkipRepro -and (Test-Path $repro)) {
        Write-Host '[4/4] resampling guard against the fuzz repro ...'
        if (-not (Test-Path $fuzzExe)) {
            & gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-function -Isrc -Itests `
                -o $fuzzExe tests\fuzz_snd.c src\snd.c -lm
            if ($LASTEXITCODE -ne 0) { throw 'building fuzz_snd failed' }
        }
        $reproOut = & $fuzzExe $repro 2>&1 | Out-String
        $reproExit = $LASTEXITCODE
        Write-Host $reproOut
        if ($reproExit -ne 1 -or $reproOut -notmatch 'RESULT REJECT') {
            $code = 4
            throw 'the rate-1 WAV repro was not rejected'
        }
        if ($reproOut -match 'oom=1' -or $reproOut -match 'peak=[0-9]{7,}') {
            $code = 4
            throw 'the repro still allocates a huge buffer'
        }
    }

    Write-Host 'RESULT: PASS -- audio plays, the mix is non-silent, byte-exact cases unchanged'
} catch {
    Write-Error $_
    if ($code -eq 0) { $code = 2 }
    Write-Host "RESULT: FAIL -- $($_.Exception.Message)"
} finally {
    $ErrorActionPreference = $savedEap
    Pop-Location
}
exit $code