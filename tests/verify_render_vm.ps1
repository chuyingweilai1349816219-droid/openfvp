<#
.SYNOPSIS
    End-to-end verification: the C script VM paints through the C renderer.

.DESCRIPTION
    1. builds bin\fvm.exe (make all) and bin\render_vm.exe (direct gcc call,
       the Makefile is intentionally not changed) from the current sources;
    2. bin\render_vm.exe runs iroseka.hcb for -At steps with the gfx context
       attached (fvp_vm_attach_gfx), calls gfx_render every -FrameSteps VM
       steps, exports out\c\vm-frame.png + out\c\vm-geom.json + vm-text.json
       + vm-render-perf.json and also a "no hold-text" variant;
    3. tests\vm_compare.py diffs the frame against
       out\render\story_<At>.png (same statistics as tests\gfx_compare.py);
    4. tests\compare.py re-runs the two byte-exact acceptance cases
       (900k and 2M steps) to prove bin\fvm.exe still behaves exactly as
       before (the renderer is optional and fvm.exe never attaches it).

    Exit codes: 0 = pass, 1 = build/setup failure, 2 = geometry mismatch,
                3 = quality below threshold, 4 = byte-exact compare failed,
                5 = render_vm failed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_render_vm.ps1

.EXAMPLE
    # reuse the already built binaries and frame, skip the two byte compares
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_render_vm.ps1 -SkipBuild -SkipByteCompare
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [string]$Script = '',
    [int]$At = 528000,
    [int]$Click = 4000,
    [int]$FrameSteps = 1000,
    [int]$HoldText = 30000,
    [string]$Out = 'out\c',
    [switch]$SkipBuild,
    [switch]$SkipByteCompare,
    [double]$MinPsnr = 40.0,
    [string]$Python = 'C:\Users\Mechrevo\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
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

$frame = Join-Path $Out 'vm-frame.png'
$nohold = Join-Path $Out 'vm-frame-nohold.png'
$geom = Join-Path $Out 'vm-geom.json'
$diffJson = Join-Path $Out 'vm-diff.json'
$perfJson = Join-Path $Out 'vm-render-perf.json'
$renderExe = Join-Path $root 'bin\render_vm.exe'

Push-Location $root
$code = 0
try {
    # native tools write progress to stderr; do not turn that into a crash
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    New-Item -ItemType Directory -Force -Path $Out | Out-Null

    if (-not $SkipBuild) {
        Write-Host '[1/5] building bin\fvm.exe ...'
        & make all
        if ($LASTEXITCODE -ne 0) { throw 'make all failed' }

        Write-Host '[2/5] building bin\render_vm.exe (direct gcc, no Makefile change) ...'
        & gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv -Isrc `
            -o $renderExe tests\render_vm.c `
            src\fvp.c src\encoding.c src\hcb.c src\vm.c src\syscall.c src\flags.c `
            src\gfx.c src\bin.c src\hzc1.c src\inflate.c`
            src\textattr.c src\save.c src\save_vm.c src\thumb.c src\timer.c src\history.c -lm
        if ($LASTEXITCODE -ne 0) { throw 'building render_vm.exe failed' }
    }
    if (-not (Test-Path $renderExe)) {
        throw "missing $renderExe (build without -SkipBuild)"
    }

    Write-Host ('[3/5] running the VM + renderer for {0} steps ...' -f $At)
    & $renderExe $Script --game $Game --steps $At --click $Click `
        --frame-steps $FrameSteps --hold-text $HoldText --out $Out
    if ($LASTEXITCODE -ne 0) { throw "render_vm.exe failed (exit $LASTEXITCODE)" }
    if (-not (Test-Path $frame)) { throw "missing $frame" }
    if (-not (Test-Path $geom)) { throw "missing $geom" }

    Write-Host '[4/5] comparing with the reference frame ...'
    & $Python 'tests\vm_compare.py' --out $Out
    if ($LASTEXITCODE -ne 0) { throw 'vm_compare.py failed' }

    $diff = [System.IO.File]::ReadAllText((Resolve-Path $diffJson),
        [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $geomOk = [bool]$diff.geometry.identical
    $posErr = $diff.geometry.position_error_px
    $psnr = [double]$diff.pixels.psnr_rgb_db
    $ssim = [double]$diff.pixels.ssim_luma

    Write-Host ''
    Write-Host '============== render_vm end-to-end verification =============='
    Write-Host ('frame           {0}  (step {1}, {2}x{3})' -f $frame, $diff.step, $diff.size[0], $diff.size[1])
    Write-Host ('draw list       {0}/{1} sprites, position error {2} px' -f `
        $diff.geometry.c_sprites, $diff.geometry.reference_sprites, $posErr)
    Write-Host ('pixels          {0} differing ({1}%), max diff {2}' -f `
        $diff.pixels.differing_rgb, $diff.pixels.percent_differing_rgb, $diff.pixels.max_rgb_diff)
    Write-Host ('quality         PSNR(rgb) {0:N2} dB, SSIM(luma) {1:N5}' -f $psnr, $ssim)
    Write-Host ('text areas      {0} differing px inside, {1} outside (max {2})' -f `
        $diff.text_area.differing_px, $diff.text_area.outside_differing_px, $diff.text_area.outside_max_diff)

    if (Test-Path $nohold) {
        & $Python 'tests\vm_compare.py' --frame $nohold --diff-name vm-diff-nohold `
            --out $Out --quiet
        $nd = [System.IO.File]::ReadAllText((Resolve-Path (Join-Path $Out 'vm-diff-nohold.json')),
            [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        Write-Host ('no hold-text    {0} differing px, PSNR {1:N2} dB (hold-text snapshot convention is render-only)' -f `
            $nd.pixels.differing_rgb, [double]$nd.pixels.psnr_rgb_db)
    }

    if (Test-Path $perfJson) {
        $perf = [System.IO.File]::ReadAllText((Resolve-Path $perfJson),
            [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        Write-Host ('perf            VM only {0:N1} ms; VM + {1} frames {2:N0} ms ({3:N2} ms/frame); gfx_render avg {4:N2} ms, hot avg {5:N2} ms, max {6:N0} ms' -f `
            [double]$perf.vm_only_ms, [int]$perf.frames, [double]$perf.vm_plus_render_ms, `
            [double]$perf.avg_frame_ms, [double]$perf.render_avg_ms, `
            [double]$perf.render_hot_avg_ms, [double]$perf.render_max_ms)
    }

    if (-not $geomOk) {
        Write-Host 'RESULT: FAIL (draw list differs from the Python reference)'
        $code = 2
    } elseif ($psnr -lt $MinPsnr) {
        Write-Host ('RESULT: FAIL (PSNR {0:N2} dB below -MinPsnr {1:N2})' -f $psnr, $MinPsnr)
        $code = 3
    } elseif ([int]$posErr -ne 0) {
        Write-Host 'RESULT: FAIL (position error != 0 px)'
        $code = 2
    } else {
        Write-Host 'RESULT: PASS (end-to-end frame matches the reference)'
    }

    if (-not $SkipByteCompare) {
        Write-Host ''
        Write-Host '[5/5] re-running the byte-exact VM acceptance cases ...'
        & $Python 'tests\compare.py' --run --quiet
        $ok900 = ($LASTEXITCODE -eq 0)
        & $Python 'tests\compare.py' --run --steps 2000000 --quiet
        $ok2m = ($LASTEXITCODE -eq 0)
        if ($ok900) {
            Write-Host 'byte-exact 900k steps  PASS'
        } else {
            Write-Host 'byte-exact 900k steps  FAIL'
            if ($code -eq 0) { $code = 4 }
        }
        if ($ok2m) {
            Write-Host 'byte-exact 2M steps    PASS'
        } else {
            Write-Host 'byte-exact 2M steps    FAIL'
            if ($code -eq 0) { $code = 4 }
        }
    }
    Write-Host '=============================================================='
} catch {
    $ErrorActionPreference = 'Continue'
    Write-Host ("RESULT: FAIL ({0})" -f $_.Exception.Message)
    $code = 1
} finally {
    $ErrorActionPreference = 'Continue'
    Pop-Location
}
exit $code