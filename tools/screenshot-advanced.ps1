# 展开「高级设置」后截图，输出到 build/shot_adv.png
# 用法：powershell -ExecutionPolicy Bypass -File tools\screenshot-advanced.ps1
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W2 {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public static List<IntPtr> All(IntPtr root) {
    var found = new List<IntPtr>();
    EnumChildWindows(root, delegate(IntPtr h, IntPtr p) { found.Add(h); return true; }, IntPtr.Zero);
    return found;
  }
  public static string TextOf(IntPtr h) {
    var sb = new StringBuilder(256);
    GetWindowTextW(h, sb, 256);
    return sb.ToString();
  }
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

$adv = @([W2]::All($h) | Where-Object { [W2]::TextOf($_) -eq "高级设置" })
if ($adv.Count -eq 0) {
  $byId = [W2]::GetDlgItem($h, 202)
  if ($byId -ne [IntPtr]::Zero) { $adv = @($byId) }
}
if ($adv.Count -eq 0) { Write-Output "advanced button not found"; Stop-Process -Id $p.Id -Force; exit 1 }
$BM_CLICK = 0x00F5
[W2]::SendMessageW($adv[0], $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1

$cr = New-Object W2+RECT
[W2]::GetClientRect($h, [ref]$cr) | Out-Null
$wr = New-Object W2+RECT
[W2]::GetWindowRect($h, [ref]$wr) | Out-Null
$origin = New-Object W2+POINT
[W2]::ClientToScreen($h, [ref]$origin) | Out-Null
$offX = $origin.X - $wr.L
$offY = $origin.Y - $wr.T
$fullW = $wr.R - $wr.L
$fullH = $wr.B - $wr.T
$full = New-Object System.Drawing.Bitmap($fullW, $fullH)
$gf = [System.Drawing.Graphics]::FromImage($full)
$hdc = $gf.GetHdc()
[W2]::PrintWindow($h, $hdc, 2) | Out-Null
$gf.ReleaseHdc($hdc)
$gf.Dispose()

$w = $cr.R; $ht = $cr.B
if (($offX + $w) -gt $fullW) { $w = $fullW - $offX }
if (($offY + $ht) -gt $fullH) { $ht = $fullH - $offY }
$client = New-Object System.Drawing.Bitmap($w, $ht)
$gc = [System.Drawing.Graphics]::FromImage($client)
$gc.DrawImage($full, (New-Object System.Drawing.Rectangle(0, 0, $w, $ht)),
              (New-Object System.Drawing.Rectangle($offX, $offY, $w, $ht)),
              [System.Drawing.GraphicsUnit]::Pixel)
$gc.Dispose(); $full.Dispose()
$out = Join-Path $root "build\shot_adv.png"
$client.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$client.Dispose()
Write-Output "$w x $ht -> $out"
Stop-Process -Id $p.Id -Force
