# Build fvp\bin\fvm.exe with the bundled w64devkit gcc.
#
#   powershell -ExecutionPolicy Bypass -File fvp\build.ps1
#
# One command: builds the C VM and creates fvp\bin\fvm.exe (plus fvp\out\c).
$ErrorActionPreference = 'Stop'

$Fvp = Split-Path -Parent $MyInvocation.MyCommand.Path
$DevkitBin = Join-Path (Split-Path -Parent $Fvp) 'tools\w64devkit\w64devkit\bin'
$Gcc = Join-Path $DevkitBin 'gcc.exe'
$Make = Join-Path $DevkitBin 'mingw32-make.exe'

if (-not (Test-Path $Gcc)) {
    throw "gcc not found at $Gcc"
}

$env:PATH = "$DevkitBin;$env:PATH"
New-Item -ItemType Directory -Force -Path (Join-Path $Fvp 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Fvp 'out\c') | Out-Null

Push-Location $Fvp
try {
    if (Test-Path $Make) {
        & $Make -f Makefile @args
    } else {
        # No make available: compile in one shot.
        $src = @('src\fvp.c', 'src\encoding.c', 'src\hcb.c', 'src\vm.c',
                 'src\syscall.c', 'src\main.c')
        & $Gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -o bin\fvm.exe @src
    }
    if ($LASTEXITCODE -ne 0) {
        throw "build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "built: $(Join-Path $Fvp 'bin\fvm.exe')"