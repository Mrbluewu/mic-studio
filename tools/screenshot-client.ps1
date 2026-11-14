# 只截客户区（不含标题栏与边框），输出到 build/shot_client.png
# 用法：powershell -ExecutionPolicy Bypass -File tools\screenshot-client.ps1
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
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
$r = New-Object W+RECT
[W]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.R; $ht = $r.B
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# PW_CLIENTONLY = 1：只画客户区，坐标与布局一致
[W]::PrintWindow($h, $hdc, 1) | Out-Null
$g.ReleaseHdc($hdc)
$out = Join-Path $root "build\shot_client.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "$w x $ht -> $out"
Stop-Process -Id $p.Id -Force
