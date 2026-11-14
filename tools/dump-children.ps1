# 打印主窗口全部子控件的句柄 / 类名 / 文本，用于排查控件是否创建成功
# 用法：powershell -ExecutionPolicy Bypass -File tools\dump-children.ps1
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W3 {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public static List<string> Dump(IntPtr root) {
    var list = new List<string>();
    EnumChildWindows(root, delegate(IntPtr h, IntPtr p) {
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      list.Add(h.ToString() + " | " + c.ToString() + " | " + t.ToString());
      return true;
    }, IntPtr.Zero);
    return list;
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
[W3]::Dump($h) | ForEach-Object { Write-Output $_ }
Stop-Process -Id $p.Id -Force
