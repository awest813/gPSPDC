param(
  [Parameter(Mandatory=$true)][string]$Cdi,
  [Parameter(Mandatory=$true)][string]$Tag,
  [int]$Seconds = 60,
  [int]$ShotEvery = 0,
  [int]$FpsEvery = 0,
  [string]$FlycastExe = $env:FLYCAST_EXE,
  [string]$OutDir = (Join-Path $env:TEMP 'gpspdc-flycast')
)
# Run a disc in Flycast unattended, take window screenshots and crops of
# Flycast's frame counter, then close it. Writes <Tag>-NNN.png, <Tag>-final.png
# and <Tag>-fps.png (one labelled row per sample) to $OutDir.
#
# Enable the counter once in Flycast's emu.cfg:  [config] rend.ShowFPS = yes
# The Dreamcast build presents one flip per emulated GBA frame, so the counter
# reads the GBA frame rate (until frameskip starts dropping frames). Do not
# touch the window during a run: input changes the attract-mode sequence.
#
# Example:
#   powershell -File scripts\flycast-run.ps1 -Cdi C:\tmp\spf.cdi -Tag spf `
#     -Seconds 91 -FpsEvery 7 -FlycastExe C:\flycast\flycast.exe
$ErrorActionPreference = 'Stop'
if (-not $FlycastExe -or -not (Test-Path $FlycastExe)) {
  throw "Pass -FlycastExe or set FLYCAST_EXE to flycast.exe"
}
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class GpspdcWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

function Capture($proc) {
  $proc.Refresh()
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { return $null }
  $r = New-Object GpspdcWin+RECT
  [void][GpspdcWin]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -le 0 -or $hh -le 0) { return $null }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [void][GpspdcWin]::PrintWindow($h, $hdc, 2)
  $g.ReleaseHdc($hdc); $g.Dispose()
  return $bmp
}

$fpsRows = New-Object System.Collections.ArrayList
# Flycast writes flycast.log into its working directory.
$p = Start-Process -FilePath $FlycastExe -ArgumentList @("`"$Cdi`"") -WorkingDirectory $OutDir -PassThru

$elapsed = 0
try {
  while ($elapsed -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds 1
    $elapsed++
    $wantShot = ($ShotEvery -gt 0 -and ($elapsed % $ShotEvery) -eq 0)
    $wantFps  = ($FpsEvery -gt 0 -and ($elapsed % $FpsEvery) -eq 0)
    if (-not ($wantShot -or $wantFps)) { continue }
    try {
      $bmp = Capture $p
      if ($bmp -eq $null) { continue }
      if ($wantShot) { $bmp.Save((Join-Path $OutDir ("{0}-{1:D3}.png" -f $Tag, $elapsed)), [System.Drawing.Imaging.ImageFormat]::Png) }
      # The counter sits in the bottom-left corner of the client area. Skip
      # minimized or still-opening windows, which are too small to hold it.
      if ($wantFps -and $bmp.Width -ge 300 -and $bmp.Height -ge 200) {
        $crop = New-Object System.Drawing.Bitmap 150, 44
        $cg = [System.Drawing.Graphics]::FromImage($crop)
        $cg.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0, 0, 150, 44),
          (New-Object System.Drawing.Rectangle 0, ($bmp.Height - 48), 150, 44),
          [System.Drawing.GraphicsUnit]::Pixel)
        $cg.Dispose()
        [void]$fpsRows.Add(@($elapsed, $crop))
      }
      $bmp.Dispose()
    } catch {
      "capture at $elapsed s failed: $($_.Exception.Message)"
    }
  }
  if (-not $p.HasExited) {
    $bmp = Capture $p
    if ($bmp -ne $null) { $bmp.Save((Join-Path $OutDir "$Tag-final.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose() }
  }
} finally {
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}

if ($fpsRows.Count -gt 0) {
  $strip = New-Object System.Drawing.Bitmap 230, (44 * $fpsRows.Count)
  $g = [System.Drawing.Graphics]::FromImage($strip)
  $g.Clear([System.Drawing.Color]::White)
  $font = New-Object System.Drawing.Font 'Consolas', 14
  for ($i = 0; $i -lt $fpsRows.Count; $i++) {
    $y = 44 * $i
    $g.DrawString(("t={0,3}s" -f $fpsRows[$i][0]), $font, [System.Drawing.Brushes]::Black, 2, ($y + 10))
    $g.DrawImage($fpsRows[$i][1], 80, $y)
  }
  $g.Dispose()
  $strip.Save((Join-Path $OutDir "$Tag-fps.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $strip.Dispose()
}
"done after $elapsed s; fps samples=$($fpsRows.Count); output in $OutDir"
