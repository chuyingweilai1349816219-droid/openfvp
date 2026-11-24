# capture-original.ps1 -- grab ground-truth frames from the ORIGINAL FVP engine.
#
# Why this exists: our reimplementation can only be validated against the real
# engine's output.  This script starts the shipping *.exe, optionally clicks
# through the title menu, and captures the window with
# PrintWindow(PW_RENDERFULLCONTENT) -- the only method that reliably returns the
# D3D9 back buffer (CopyFromScreen gave an all-black bitmap).
#
# It also dumps Win32 message boxes (#32770) with their child control texts.
# That is how the "game exits after ~6 s" mystery was solved: the engine writes a
# system save to
#     %USERPROFILE%\Documents\FAVORITE\<title>\save\s901.bin
# on startup, and if that path is missing or not writable it shows
#     '<path>' no sakusei ni shippai shimashita   (cp932; shows up as mojibake via GetWindowText)
# and then quits with exit code 0.  The directory must exist AND the process that
# launches the game must be allowed to write there, otherwise the child inherits
# the restriction and the game still fails.
#
# Captured PNGs are game content: keep them out of the repository (out/ is
# gitignored).  Only measured numbers belong in docs/.
#
# Usage (needs full filesystem access for the game's Documents write):
#   powershell -ExecutionPolicy Bypass -File scripts\capture-original.ps1 `
#       -GameDir 'D:\games\iroseka HD' -Clicks 1 -CaptureSec 60
#
# Parameters:
#   -GameDir     directory containing FVPLoaderGui.exe / <Exe> (auto-detected under
#                Downloads if omitted)
#   -Exe         game executable (default iroseka_HD.exe)
#   -TitleWaitSec seconds to wait for the title screen before clicking (default 13)
#   -Clicks      how many left clicks at the New Game hit point (0 = none)
#   -CaptureSec  seconds of frames to capture after the click (default 20)
#   -ClickEvery  while capturing, click the centre of the client area every N
#                seconds to advance dialogue (default 0 = never)
#   -Park        what to do with the game window so it does not take over the
#                desktop: offscreen (default; PrintWindow keeps working) |
#                minimize (saves the window but capture is useless) | none
#   -OutDir      output directory (default <repo>\out\orig-capture)
#   -IntervalMs  frame interval (default 900 ms)

param(
    [string]$GameDir = '',
    [string]$Exe = 'iroseka_HD.exe',
    [int]$TitleWaitSec = 13,
    [int]$Clicks = 1,
    [int]$CaptureSec = 20,
    [int]$ClickEvery = 0,
    [string]$Park = 'offscreen',
    [string]$OutDir = '',
    [int]$IntervalMs = 900
)
$ErrorActionPreference = 'Continue'

$src = @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class FvpCap {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }

  public static List<IntPtr> Top(uint want) {
    var o = new List<IntPtr>();
    EnumWindows((h,p) => { uint pid; GetWindowThreadProcessId(h, out pid); if (pid == want) o.Add(h); return true; }, IntPtr.Zero);
    return o;
  }
  public static string Cls(IntPtr h) { var c = new StringBuilder(256); GetClassName(h, c, 256); return c.ToString(); }
  public static string Txt(IntPtr h) { var t = new StringBuilder(2048); GetWindowTextW(h, t, 2048); return t.ToString(); }
  public static List<string> Kids(IntPtr h) {
    var o = new List<string>();
    EnumChildWindows(h, (c,p) => {
      var t = new StringBuilder(4096); GetWindowTextW(c, t, 4096);
      o.Add(string.Format("child class={0} text=[{1}]", Cls(c), t));
      return true;
    }, IntPtr.Zero);
    return o;
  }
  // The engine always renders a 1920x1080 scene, letterboxed inside whatever
  // client size the DPI-scaled window ends up with (1707x1067 and 1926x1109 both
  // occur), so clicks must be derived from the live client rect.
  public static POINT GameToScreen(IntPtr h, int gx, int gy) {
    RECT c; GetClientRect(h, out c);
    double cw = c.R - c.L, ch = c.B - c.T;
    double scale = cw / 1920.0;
    double off = (ch - 1080.0 * scale) / 2.0;
    if (off < 0) off = 0;
    var p = new POINT();
    p.X = (int)Math.Round(gx * scale);
    p.Y = (int)Math.Round(off + gy * scale);
    ClientToScreen(h, ref p);
    return p;
  }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(150);
    mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
    System.Threading.Thread.Sleep(90);
    mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
  }
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int idx);
  // The engine pins its client area to 1920x1080: its window has no WS_THICKFRAME
  // and it re-forces the size on WM_WINDOWPOSCHANGING, so SetWindowPos cannot
  // shrink it.  But it CAN be moved -- and PrintWindow still returns live content
  // for a window parked outside the desktop (verified: 99.2% non-black, same as
  // on-screen), while a minimized window reports a 237x39 icon-sized bitmap.
  // So "get it out of the user's way" == move it off the desktop, not resize it.
  public static void ParkOffscreen(IntPtr h) {
    int w = GetSystemMetrics(0);              // virtualized desktop width
    SetWindowPos(h, IntPtr.Zero, w + 40, 40, 0, 0,
                 0x0001 /*NOSIZE*/ | 0x0004 /*NOZORDER*/ | 0x0010 /*NOACTIVATE*/);
  }
  public static void BringOnscreen(IntPtr h) {
    SetWindowPos(h, IntPtr.Zero, 0, 0, 0, 0,
                 0x0001 /*NOSIZE*/ | 0x0004 /*NOZORDER*/);
  }
  public static void Minimize(IntPtr h) { ShowWindow(h, 6 /* SW_MINIMIZE */); }
}
"@
Add-Type -TypeDefinition $src -Language CSharp
Add-Type -AssemblyName System.Drawing

# Keep the original out of the way.  Resizing is not possible (the engine pins a
# 1920x1080 client and re-forces it), so we move the window off the desktop --
# PrintWindow still captures live frames from there.
$parkMode = $Park.ToLowerInvariant()
if ($parkMode -notin @('offscreen', 'minimize', 'none')) {
    Write-Host "bad -Park '$Park' (want offscreen|minimize|none)"; exit 2
}

function Park-Window([IntPtr]$h) {
    switch ($parkMode) {
        'offscreen' { [FvpCap]::ParkOffscreen($h) }
        'minimize'  { [FvpCap]::Minimize($h) }
        default     { }
    }
}

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($OutDir -eq '') { $OutDir = Join-Path $root 'out\orig-capture' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

if ($GameDir -eq '') {
    $GameDir = (Get-ChildItem 'C:\Users\Mechrevo\Downloads' -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName 'FVPLoaderGui.exe') } |
                Select-Object -First 1).FullName
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir $Exe))) {
    Write-Host "cannot find $Exe (pass -GameDir)"; exit 2
}
$game = Join-Path $GameDir $Exe

function Find-Main([uint32]$procId) {
    # NB: never name a parameter $pid -- it is a read-only automatic variable.
    foreach ($h in [FvpCap]::Top($procId)) {
        if ([FvpCap]::Cls($h) -eq 'HS_MAIN_WINDOW_CLASS00') { return $h }
    }
    return [IntPtr]::Zero
}

function Capture([IntPtr]$h, [string]$path) {
    $rr = New-Object FvpCap+RECT
    if (-not [FvpCap]::GetWindowRect($h, [ref]$rr)) { return $null }
    $w = $rr.R - $rr.L; $hh = $rr.B - $rr.T
    if ($w -lt 16 -or $hh -lt 16) { return $null }
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][FvpCap]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    # 24x24 sample grid -> cheap content fingerprint, used to spot idle frames
    $sum = 0
    $sx = [math]::Max(1, [int]($w / 24)); $sy = [math]::Max(1, [int]($hh / 24))
    for ($y = 0; $y -lt $hh; $y += $sy) {
        for ($x = 0; $x -lt $w; $x += $sx) {
            $c = $bmp.GetPixel($x, $y)
            $sum = ($sum + $c.R + $c.G * 3 + $c.B * 7) % 1000000007
        }
    }
    $bmp.Dispose()
    return "$w`x$hh`:$sum"
}

function DumpDialogs([uint32]$procId, $seen) {
    foreach ($h in [FvpCap]::Top($procId)) {
        if ([FvpCap]::Cls($h) -eq '#32770' -and -not $seen.ContainsKey($h)) {
            $seen[$h] = $true
            Write-Host 'DIALOG'
            foreach ($k in [FvpCap]::Kids($h)) { Write-Host "  $k" }
        }
    }
}

$p = Start-Process -FilePath $game -WorkingDirectory $GameDir -PassThru
Write-Host "pid=$($p.Id) exe=$game"
$seenDlg = @{}
$main = [IntPtr]::Zero
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# phase 1: wait for the title screen
while ($sw.Elapsed.TotalSeconds -lt $TitleWaitSec) {
    Start-Sleep -Milliseconds 400
    $p.Refresh()
    if ($p.HasExited) { Write-Host ("EXIT code={0}" -f $p.ExitCode); exit 1 }
    DumpDialogs ([uint32]$p.Id) $seenDlg
    $m = Find-Main([uint32]$p.Id)
    if ($m -ne [IntPtr]::Zero) { $main = $m; Park-Window $m }
}
if ($main -eq [IntPtr]::Zero) { Write-Host 'no main window'; try { $p.Kill() } catch {}; exit 1 }

Start-Sleep -Milliseconds 400
Park-Window $main
$pre = Capture $main (Join-Path $OutDir 'title.png')
Write-Host "title frame $pre"

# phase 2: click "New Game" (game-space centre measured on the original captures).
# The click needs the window on a real screen position, so bring it back for the
# duration of the click only, then park it again.
if ($Clicks -gt 0) {
    if ($parkMode -ne 'none') { [FvpCap]::BringOnscreen($main); Start-Sleep -Milliseconds 600 }
    [void][FvpCap]::SetForegroundWindow($main)
    Start-Sleep -Milliseconds 500
    $pt = [FvpCap]::GameToScreen($main, 1645, 552)
    for ($i = 0; $i -lt $Clicks; $i++) {
        Write-Host ("click {0}/{1} at screen {2},{3}" -f ($i + 1), $Clicks, $pt.X, $pt.Y)
        [FvpCap]::Click($pt.X, $pt.Y)
        Start-Sleep -Milliseconds 700
    }
}

# phase 3: capture
$n = 0; $last = ''
$nextClick = if ($ClickEvery -gt 0) { [double]$ClickEvery } else { [double]::MaxValue }
$sw2 = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw2.Elapsed.TotalSeconds -lt $CaptureSec) {
    Start-Sleep -Milliseconds $IntervalMs
    $p.Refresh()
    if ($p.HasExited) { Write-Host ("EXIT code={0}" -f $p.ExitCode); break }
    DumpDialogs ([uint32]$p.Id) $seenDlg
    $m = Find-Main([uint32]$p.Id)
    if ($m -eq [IntPtr]::Zero) { continue }
    # advance the dialogue: bring the window back on screen, click, park again
    if ($sw2.Elapsed.TotalSeconds -ge $nextClick) {
        $nextClick += $ClickEvery
        if ($parkMode -ne 'none') { [FvpCap]::BringOnscreen($m); Start-Sleep -Milliseconds 250 }
        $pt = [FvpCap]::GameToScreen($m, 960, 540)
        [FvpCap]::Click($pt.X, $pt.Y)
        Write-Host ("click advance at t={0:N1}s" -f $sw2.Elapsed.TotalSeconds)
        if ($parkMode -ne 'none') { Start-Sleep -Milliseconds 150; Park-Window $m }
    }
    Park-Window $m   # keep the window off the desktop between clicks
    $n++
    $name = Join-Path $OutDir ("frame-{0:D3}.png" -f $n)
    $fp = Capture $m $name
    if ($null -eq $fp) { continue }
    $mark = ''
    if ($fp -ne $last) { $mark = ' CHANGED' }
    $last = $fp
    Write-Host ("FRAME frame-{0:D3}.png t={1:N1}s {2}{3}" -f $n, $sw2.Elapsed.TotalSeconds, $fp, $mark)
}
$p.Refresh()
if (-not $p.HasExited) { Write-Host 'killing'; try { $p.Kill() } catch {} }
Write-Host "done -> $OutDir"