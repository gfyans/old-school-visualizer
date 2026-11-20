<#
  Regenerates res\app.ico: a small glowing nebula with stars on a dark rounded square.
  Sizes 16-256, each stored as PNG inside the .ico (supported since Windows Vista).
    powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$outPath = Join-Path $root 'res\app.ico'

function New-Blob($g, [float]$cx, [float]$cy, [float]$r, [Drawing.Color]$inner) {
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($cx - $r, $cy - $r, 2 * $r, 2 * $r)
    $brush = New-Object Drawing.Drawing2D.PathGradientBrush $path
    $brush.CenterColor = $inner
    $brush.SurroundColors = @([Drawing.Color]::FromArgb(0, $inner.R, $inner.G, $inner.B))
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}

function Draw-Icon([int]$size) {
    $s = $size / 256.0
    $bmp = New-Object Drawing.Bitmap $size, $size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.Clear([Drawing.Color]::Transparent)

    # Dark rounded-square background.
    $r = 56 * $s; $d = 2 * $r; $m = 8 * $s; $w = $size - 2 * $m
    $bg = New-Object Drawing.Drawing2D.GraphicsPath
    $bg.AddArc($m, $m, $d, $d, 180, 90); $bg.AddArc($m + $w - $d, $m, $d, $d, 270, 90)
    $bg.AddArc($m + $w - $d, $m + $w - $d, $d, $d, 0, 90); $bg.AddArc($m, $m + $w - $d, $d, $d, 90, 90); $bg.CloseFigure()
    $bgBrush = New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.PointF]::new(0, 0)), ([Drawing.PointF]::new($size, $size)),
        ([Drawing.Color]::FromArgb(255, 14, 10, 32)), ([Drawing.Color]::FromArgb(255, 4, 6, 16))
    $g.FillPath($bgBrush, $bg)
    $g.SetClip($bg)

    # Nebula: overlapping soft glows.
    New-Blob $g (100 * $s) (150 * $s) (110 * $s) ([Drawing.Color]::FromArgb(200, 150, 40, 170))
    New-Blob $g (165 * $s) (95 * $s) (95 * $s) ([Drawing.Color]::FromArgb(190, 30, 150, 190))
    New-Blob $g (150 * $s) (170 * $s) (70 * $s) ([Drawing.Color]::FromArgb(170, 240, 110, 80))
    New-Blob $g (128 * $s) (128 * $s) (48 * $s) ([Drawing.Color]::FromArgb(255, 255, 225, 200))
    New-Blob $g (128 * $s) (128 * $s) (18 * $s) ([Drawing.Color]::FromArgb(255, 255, 255, 255))

    # Stars (skipped at tiny sizes where they'd just be noise).
    if ($size -ge 32) {
        $rng = New-Object Random 7
        for ($i = 0; $i -lt 18; $i++) {
            $x = $rng.Next(20, 236) * $s; $y = $rng.Next(20, 236) * $s; $rad = (1.2 + $rng.NextDouble() * 2.2) * $s
            $a = 140 + $rng.Next(0, 115)
            $g.FillEllipse((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb($a, 255, 255, 255))), $x - $rad, $y - $rad, 2 * $rad, 2 * $rad)
        }
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($size in $sizes) {
    $bmp = Draw-Icon $size
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    , $ms.ToArray()
}

New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
$fs = [IO.File]::Create($outPath); $bw = New-Object IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                        # ICONDIRENTRY
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($png in $pngs) { $bw.Write($png) }
$bw.Close()
Write-Host "Wrote $outPath ($((Get-Item $outPath).Length) bytes)"
