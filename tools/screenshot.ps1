# 截取主窗口（含标题栏）完整外观，输出到 build/shot.png
# 用法：powershell -ExecutionPolicy Bypass -File tools\screenshot.ps1
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
}
"@
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\apps\micstudio\Release\micstudio.exe"
$p = Start-Process $exe -ArgumentList "--show" -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 250
  $p.Refresh()
  if ($p.HasExited) { Write-Output "exited early"; exit 1 }
  if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
Start-Sleep -Seconds 1

$cr = New-Object W+RECT
[W]::GetClientRect($h, [ref]$cr) | Out-Null
$wr = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$wr) | Out-Null
$origin = New-Object W+POINT
$origin.X = 0; $origin.Y = 0
[W]::ClientToScreen($h, [ref]$origin) | Out-Null
$offX = $origin.X - $wr.L
$offY = $origin.Y - $wr.T

$fullW = $wr.R - $wr.L
$fullH = $wr.B - $wr.T
$full = New-Object System.Drawing.Bitmap($fullW, $fullH)
$gf = [System.Drawing.Graphics]::FromImage($full)
$hdc = $gf.GetHdc()
[W]::PrintWindow($h, $hdc, 2) | Out-Null
$gf.ReleaseHdc($hdc)
$gf.Dispose()

$w = $cr.R
$ht = $cr.B
if (($offX + $w) -gt $fullW) { $w = $fullW - $offX }
if (($offY + $ht) -gt $fullH) { $ht = $fullH - $offY }
$client = New-Object System.Drawing.Bitmap($w, $ht)
$gc = [System.Drawing.Graphics]::FromImage($client)
$gc.DrawImage($full, (New-Object System.Drawing.Rectangle(0, 0, $w, $ht)),
              (New-Object System.Drawing.Rectangle($offX, $offY, $w, $ht)),
              [System.Drawing.GraphicsUnit]::Pixel)
$gc.Dispose()
$full.Dispose()

$out = Join-Path $root "build\shot.png"
$client.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$client.Dispose()
Write-Output "$w x $ht (offset $offX,$offY) -> $out"
Stop-Process -Id $p.Id -Force
