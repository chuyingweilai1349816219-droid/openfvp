<#
.SYNOPSIS
    Verify the C gfx renderer against the proven Python reference frame.

.DESCRIPTION
    1. fvp/tests/gfx_dump.py runs the Python renderer (work/render.py) to a
       checkpoint and dumps the complete frame state to out\c\gfx-scene.json
       (plus the bitmap font atlases and a Python re-render for sanity).
    2. bin/test_gfx.exe replays that state through src/gfx.c, renders the
       frame, exports out\c\gfx-frame.png + out\c\gfx-geom.json and checks its
       draw list against the reference draw list (0 px position error).
    3. fvp/tests/gfx_compare.py diffs the C frame against
       out\render\story_<step>.png and writes out\c\gfx-diff.json.

    Exit codes: 0 = pass, 2 = geometry mismatch, 3 = quality below threshold.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_gfx.ps1

.EXAMPLE
    # reuse an existing dump, only rebuild + compare
    powershell -ExecutionPolicy Bypass -File fvp\tests\verify_gfx.ps1 -SkipDump
#>
[CmdletBinding()]
param(
    [string]$Game = 'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
    [int]$At = 528000,
    [string]$Out = 'out\c',
    [switch]$SkipDump,
    [switch]$SkipBuild,
    [double]$MinPsnr = 40.0,
    [int]$MaxPositionErrorPx = 0,
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

$scene = Join-Path $Out 'gfx-scene.json'
$frame = Join-Path $Out 'gfx-frame.png'
$geom = Join-Path $Out 'gfx-geom.json'
$diffJson = Join-Path $Out 'gfx-diff.json'
$log = Join-Path $Out 'gfx-test.log'

Push-Location $root
$code = 0
try {
    # native tools write progress to stderr; do not turn that into a crash
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    if (-not $SkipDump) {
        Write-Host '[1/4] running the Python reference to step' $At '...'
        & $Python 'tests\gfx_dump.py' --at $At --out $Out --game $Game
        if ($LASTEXITCODE -ne 0) { throw 'gfx_dump.py failed' }
    } elseif (-not (Test-Path $scene)) {
        throw "missing $scene (run once without -SkipDump)"
    }

    if (-not $SkipBuild) {
        Write-Host '[2/4] building bin\test_gfx.exe ...'
        & make test_gfx
        if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    }
    if (-not (Test-Path 'bin\test_gfx.exe')) {
        throw 'bin\test_gfx.exe missing (build first)'
    }

    Write-Host '[3/4] rendering the frame with src\gfx.c ...'
    & 'bin\test_gfx.exe' $scene -o $Out
    $renderExit = $LASTEXITCODE
    $ErrorActionPreference = $savedEap

    if (-not (Test-Path $diffJson) -or $renderExit -eq 2) {
        # geometry mismatch: still run the compare so the report exists
        if ($renderExit -eq 2) { Write-Warning 'test_gfx reported a geometry mismatch' }
    }
    if (-not (Test-Path $frame)) { throw "missing $frame" }
    if (-not (Test-Path $geom)) { throw "missing $geom" }

    Write-Host '[4/4] comparing with the reference PNG ...'
    $ErrorActionPreference = 'Continue'
    & $Python 'tests\gfx_compare.py' --scene $scene --out $Out
    if ($LASTEXITCODE -ne 0) { throw 'gfx_compare.py failed' }
    $ErrorActionPreference = $savedEap

    $diff = [System.IO.File]::ReadAllText((Resolve-Path $diffJson),
        [System.Text.Encoding]::UTF8) | ConvertFrom-Json

    $geomOk = [bool]$diff.geometry.identical
    $posErr = $diff.geometry.position_error_px
    $psnr = [double]$diff.pixels.psnr_rgb_db
    $ssim = [double]$diff.pixels.ssim_luma

    Write-Host ''
    Write-Host '================ gfx port verification ================'
    Write-Host ('scene           {0}  (step {1}, {2}x{3})' -f $scene, $diff.step, $diff.size[0], $diff.size[1])
    Write-Host ('draw list       {0}/{1} sprites, position error {2} px' -f `
        $diff.geometry.c_sprites, $diff.geometry.reference_sprites, $posErr)
    Write-Host ('pixels          {0} differing ({1}%), max diff {2}' -f `
        $diff.pixels.differing_rgb, $diff.pixels.percent_differing_rgb, $diff.pixels.max_rgb_diff)
    Write-Host ('quality         PSNR(rgb) {0:N2} dB, SSIM(luma) {1:N5}' -f $psnr, $ssim)
    Write-Host ('text areas      {0} differing px inside, {1} outside (max {2})' -f `
        $diff.text_area.differing_px, $diff.text_area.outside_differing_px, $diff.text_area.outside_max_diff)
    if (Test-Path $log) {
        foreach ($line in [System.IO.File]::ReadAllLines((Resolve-Path $log),
                [System.Text.Encoding]::UTF8)) {
            if ($line -match '^(load_ms|cold_render_ms|warm_render_ms|image_loads|compo_builds|resize_cache_hits|pixels)=') {
                Write-Host ('perf            {0}' -f $line)
            }
        }
    }
    Write-Host ('artifacts       {0}, {1}, {2}' -f $frame, $diffJson,
        (Join-Path $Out 'gfx-diff.png'))

    if (-not $geomOk) {
        Write-Host 'RESULT: FAIL (draw list differs from the Python reference)'
        $code = 2
    } elseif ($psnr -lt $MinPsnr) {
        Write-Host ('RESULT: FAIL (PSNR {0:N2} dB below -MinPsnr {1:N2})' -f $psnr, $MinPsnr)
        $code = 3
    } elseif ([int]$posErr -gt $MaxPositionErrorPx) {
        Write-Host 'RESULT: FAIL (position error != 0 px)'
        $code = 2
    } else {
        Write-Host 'RESULT: PASS'
        $code = 0
    }
    Write-Host '======================================================='
} catch {
    $ErrorActionPreference = 'Continue'
    Write-Host ("RESULT: FAIL ({0})" -f $_.Exception.Message)
    $code = 1
} finally {
    Pop-Location
}
exit $code