# 把 build/shot.png 局部放大，输出 build/z_<名字>.png，便于逐像素核对布局
# 用法：powershell -ExecutionPolicy Bypass -File tools\crop.ps1
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "build\shot.png"
$img = [System.Drawing.Image]::FromFile($src)
function Zoom($name, $x, $y, $w, $h, $scale) {
  $out = New-Object System.Drawing.Bitmap(($w * $scale), ($h * $scale))
  $g = [System.Drawing.Graphics]::FromImage($out)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
  $g.DrawImage($img, (New-Object System.Drawing.Rectangle(0, 0, ($w * $scale), ($h * $scale))),
               (New-Object System.Drawing.Rectangle($x, $y, $w, $h)),
               [System.Drawing.GraphicsUnit]::Pixel)
  $g.Dispose()
  $out.Save((Join-Path $root "build\z_$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $out.Dispose()
  Write-Output "z_$name.png"
}
Zoom "header" 0 0 300 44 4
Zoom "power" 0 90 300 210 3
Zoom "bottom" 0 340 300 160 3
$img.Dispose()
