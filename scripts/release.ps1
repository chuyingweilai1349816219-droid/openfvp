#Requires -Version 5.1
<#
.SYNOPSIS
    Package the repository sources into a versioned zip for a GitHub release.

.DESCRIPTION
    Produces dist/fvp-<version>-<commit>.zip with the same content that the
    repository is meant to ship: sources, docs, tests, ref, scripts, CI config.
    Build artifacts, local scratch directories and game data are excluded using
    the same rules as .gitignore.

    The file name always carries the version (VERSION file) and a commit id
    (`git rev-parse --short HEAD`, or -Commit).  When git is unavailable the
    commit part becomes `nogit-<UTC timestamp>` and a warning is printed.

.PARAMETER OutDir
    Output directory (default: <repo>/dist).

.PARAMETER Commit
    Commit id to embed (default: from git).

.PARAMETER Force
    Overwrite an existing zip.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Commit a1b2c3d -Force
#>
[CmdletBinding()]
param(
    [string]$OutDir = '',
    [string]$Commit = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir

# --- sanity: are we at the repository root? ---------------------------------
foreach ($required in @('VERSION', 'README.md', 'LICENSE', 'Makefile', 'src', 'docs')) {
    if (-not (Test-Path (Join-Path $Root $required))) {
        throw "not a repository root: '$required' missing under $Root"
    }
}

# --- version ----------------------------------------------------------------
$VersionPath = Join-Path $Root 'VERSION'
$Version = (Get-Content -Path $VersionPath -Raw -Encoding UTF8).Trim()
if (-not $Version) { throw "VERSION file is empty: $VersionPath" }
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._+-]*$') {
    throw "VERSION '$Version' contains characters that are unsafe in a file name"
}

# --- commit -----------------------------------------------------------------
function Get-GitCommit([string]$Repo) {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return '' }
    try {
        $out = & $git.Source -C $Repo rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { return ($out | Select-Object -First 1).Trim() }
    } catch {
        return ''
    }
    return ''
}

$CommitSource = 'argument'
if (-not $Commit) {
    $CommitSource = 'git'
    $Commit = Get-GitCommit $Root
    if (-not $Commit) {
        $CommitSource = 'timestamp'
        $Commit = 'nogit-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss')
        Write-Warning 'git not available (or no commit yet); using timestamp in file name.'
    }
}
if ($Commit -notmatch '^[0-9A-Za-z._-]+$') {
    throw "commit '$Commit' contains characters that are unsafe in a file name"
}

# --- output path ------------------------------------------------------------
if (-not $OutDir) { $OutDir = Join-Path $Root 'dist' }
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
$Name = "fvp-$Version-$Commit"
$ZipPath = Join-Path $OutDir "$Name.zip"

if ((Test-Path $ZipPath) -and -not $Force) {
    throw "output already exists: $ZipPath (use -Force to overwrite)"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- exclusion rules (keep in sync with .gitignore) -------------------------
# Directories are matched on the first path segment(s) below the repo root.
$ExcludedDirs = @(
    '.git', '.svn', '.hg',
    'bin', 'out', 'extract', 'work', 'tools-src', 'dist',
    '__pycache__', '.venv', 'venv', 'node_modules', '.idea', '.vscode',
    'dsh-config-backup'
)
# File names / extensions that are always generated or local.
$ExcludedFiles = @(
    '*.o', '*.obj', '*.a', '*.lib', '*.exe', '*.dll', '*.so', '*.dylib',
    '*.pyc', '*.pyo', '*.log', '*.tmp', '*.ppm', '*.rgba',
    'Thumbs.db', 'ehthumbs.db', 'Desktop.ini', '.DS_Store'
)

function Test-Excluded([string]$RelPath) {
    $parts = @($RelPath -split '[\\/]')
    if ($parts.Count -gt 1) {
        for ($i = 0; $i -lt ($parts.Count - 1); $i++) {
            if ($ExcludedDirs -contains $parts[$i]) { return $true }
        }
    }
    $leaf = $parts[$parts.Count - 1]
    foreach ($pattern in $ExcludedFiles) {
        if ($leaf -like $pattern) { return $true }
    }
    return $false
}

# --- collect files ----------------------------------------------------------
$all = Get-ChildItem -Path $Root -Recurse -File -Force -ErrorAction SilentlyContinue
$files = New-Object System.Collections.Generic.List[object]
$skipped = 0
foreach ($f in $all) {
    $rel = $f.FullName.Substring($Root.Length).TrimStart('\', '/')
    if (Test-Excluded $rel) { $skipped++; continue }
    $files.Add([pscustomobject]@{ Full = $f.FullName; Rel = $rel })
}
if ($files.Count -eq 0) { throw "no files selected under $Root" }

# Guard rails: the package must contain the things a contributor needs.
$mustContain = @('README.md', 'README.en.md', 'LICENSE', 'VERSION', 'Makefile',
                 'CONTRIBUTING.md', 'THIRD-PARTY.md', '.gitignore',
                 '.gitattributes', 'docs\README.md', 'src\vm.c', 'src\hcb.c',
                 'src\snd.c', 'tests\compare.py', 'ref\ref-900k.txt',
                 'scripts\check-docs.py', 'scripts\ci-compare.py',
                 '.github\workflows\ci.yml')
$selected = @{}
foreach ($f in $files) { $selected[$f.Rel] = $true }
$missing = @()
foreach ($m in $mustContain) {
    if (-not $selected.ContainsKey($m)) { $missing += $m }
}
if ($missing.Count -gt 0) {
    throw ("package would be incomplete; missing: " + ($missing -join ', '))
}

# --- write the zip ----------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$utf8 = New-Object System.Text.UTF8Encoding($false)
$zip = [System.IO.Compression.ZipFile]::Open(
    $ZipPath, [System.IO.Compression.ZipArchiveMode]::Create, $utf8)
try {
    foreach ($f in $files) {
        $entryName = "$Name/" + ($f.Rel -replace '\\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $f.Full, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $zip.Dispose()
}

# --- report -----------------------------------------------------------------
$zipInfo = Get-Item $ZipPath
$zipHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash
$rawBytes = ($files | ForEach-Object { (Get-Item $_.Full).Length } |
             Measure-Object -Sum).Sum

Write-Host ''
Write-Host "release package : $ZipPath"
Write-Host "version         : $Version"
Write-Host "commit          : $Commit  (from $CommitSource)"
Write-Host "files           : $($files.Count)  ($skipped excluded)"
Write-Host "uncompressed    : $rawBytes bytes"
Write-Host "zip size        : $($zipInfo.Length) bytes"
Write-Host "zip sha256      : $zipHash"
Write-Host ''
Write-Host 'Excluded (not shipped): bin/ out/ extract/ work/ tools-src/ dist/,'
Write-Host '  *.o *.a *.exe *.pyc *.log, __pycache__, .git'
Write-Host ''
Write-Host 'RESULT: PASS'