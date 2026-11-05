# make-packer-icon.ps1 - draw the icon of rf-packer: a blue cardboard box with RF on its side
#
# Each size is drawn on its own, not scaled from another: 16 and 32 on a grid of their own, pixel by pixel; 48 and up smooth, with more in them
# (tape, light on the edges, the letters sheared with the face). Writes src/packer/Graphics/*-rf-packer.png and appicon.ico (PNG images).
# Windows PowerShell (System.Drawing).
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 RF-tools contributors

Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\packer\Graphics'
function C($hex){ [System.Drawing.ColorTranslator]::FromHtml($hex) }
function Pt($x,$y){ New-Object System.Drawing.PointF ([single]$x),([single]$y) }
$outline = C '#12304F'

# ---- the small ones: drawn on their own grid, pixel by pixel ----
function New-Pixel($n,$poly,$glyphs,$col0,$gap,$shift){
  $bmp = New-Object System.Drawing.Bitmap $n,$n,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode='None'; $g.PixelOffsetMode='Half'
  $g.Clear([System.Drawing.Color]::Transparent)
  foreach($face in $poly.faces){
    $pts = [System.Drawing.Point[]]($face.pts | ForEach-Object { New-Object System.Drawing.Point $_[0],$_[1] })
    $g.FillPolygon((New-Object System.Drawing.SolidBrush (C $face.color)),$pts)
  }
  $pen = New-Object System.Drawing.Pen $outline,1
  foreach($l in $poly.lines){ $a=$l[0]; $b=$l[1]; $g.DrawLine($pen,$a[0],$a[1],$b[0],$b[1]) }
  $g.Dispose()
  $x0 = $col0
  foreach($glyph in $glyphs){
    for($row=0;$row -lt $glyph.Count;$row++){ for($c=0;$c -lt $glyph[$row].Length;$c++){
      if($glyph[$row][$c] -eq '1'){
        $x = $x0 + $c
        $y = $poly.faceTop - [Math]::Floor(($x - $poly.faceX)/2) + $shift + $row
        $bmp.SetPixel($x,$y,(C '#F4F8FF'))
      } } }
    $x0 += $glyph[0].Length + $gap
  }
  return $bmp
}

# 32 x 32
$p32 = @{ faceTop=16; faceX=16
  faces=@(
    @{color='#A9D2F5'; pts=@(@(16,4),@(28,10),@(16,16),@(4,10))},
    @{color='#5B9BE0'; pts=@(@(4,10),@(16,16),@(16,28),@(4,22))},
    @{color='#2F6DB5'; pts=@(@(16,16),@(28,10),@(28,22),@(16,28))},
    @{color='#E4F0FC'; pts=@(@(19,5),@(24,8),@(13,14),@(8,11))} )
  lines=@( @(@(16,4),@(28,10)), @(@(28,10),@(28,22)), @(@(28,22),@(16,28)), @(@(16,28),@(4,22)), @(@(4,22),@(4,10)), @(@(4,10),@(16,4)),
           @(@(4,10),@(16,16)), @(@(28,10),@(16,16)), @(@(16,16),@(16,28)) ) }
$b32 = New-Pixel 32 $p32 @(@('1110','1001','1110','1010','1001'),@('1111','1000','1110','1000','1000')) 17 1 4
$b32.Save("$out\32-rf-packer.png",[System.Drawing.Imaging.ImageFormat]::Png)

# 16 x 16: a cube of 14 pixels, the letters as small as they can be
$p16 = @{ faceTop=8; faceX=8
  faces=@(
    @{color='#A9D2F5'; pts=@(@(8,2),@(14,5),@(8,8),@(2,5))},
    @{color='#5B9BE0'; pts=@(@(2,5),@(8,8),@(8,14),@(2,11))},
    @{color='#2F6DB5'; pts=@(@(8,8),@(14,5),@(14,11),@(8,14))},
    @{color='#E4F0FC'; pts=@(@(9,3),@(12,4),@(7,7),@(4,6))} )
  lines=@( @(@(8,2),@(14,5)), @(@(14,5),@(14,11)), @(@(14,11),@(8,14)), @(@(8,14),@(2,11)), @(@(2,11),@(2,5)), @(@(2,5),@(8,2)),
           @(@(2,5),@(8,8)), @(@(14,5),@(8,8)), @(@(8,8),@(8,14)) ) }
$b16 = New-Pixel 16 $p16 @(@('11','11','10','11'),@('11','10','10','10')) 9 1 1
$b16.Save("$out\16-rf-packer.png",[System.Drawing.Imaging.ImageFormat]::Png)

# ---- the larger ones: drawn at their own size, smooth, with more in them ----
function New-Vector($size){
  $k = $size / 256.0
  $bmp = New-Object System.Drawing.Bitmap $size,$size,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode='AntiAlias'; $g.PixelOffsetMode='HighQuality'; $g.TextRenderingHint='AntiAliasGridFit'
  $g.Clear([System.Drawing.Color]::Transparent)
  function S($x,$y){ Pt ($x*$k) ($y*$k) }
  $N=S 128 32; $E=S 224 80; $Sx=S 128 128; $W=S 32 80
  $W2=S 32 176; $S2=S 128 224; $E2=S 224 176
  function Face($pts,$c1,$c2){
    $xs = $pts | ForEach-Object { $_.X }; $ys = $pts | ForEach-Object { $_.Y }
    $r = [System.Drawing.RectangleF]::FromLTRB(($xs | Measure-Object -Minimum).Minimum,($ys | Measure-Object -Minimum).Minimum,($xs | Measure-Object -Maximum).Maximum,($ys | Measure-Object -Maximum).Maximum)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r,(C $c1),(C $c2),90
    $g.FillPolygon($br,[System.Drawing.PointF[]]$pts)
  }
  Face @($N,$E,$Sx,$W) '#DCEEFC' '#9CC8F0'
  Face @($W,$Sx,$S2,$W2) '#6EAAEA' '#4684CA'
  Face @($Sx,$E,$E2,$S2) '#3C7AC2' '#245A99'
  # the tape: across the top, and down the left face
  $tape = New-Object System.Drawing.SolidBrush (C '#EAF3FC')
  $tapeEdge = New-Object System.Drawing.Pen (C '#B5D0EE'),([single]([Math]::Max(1,2*$k)))
  $top = [System.Drawing.PointF[]]@((S 164 50),(S 188 62),(S 92 110),(S 68 98))
  $g.FillPolygon($tape,$top); $g.DrawPolygon($tapeEdge,$top)
  $side = [System.Drawing.PointF[]]@((S 68 98),(S 92 110),(S 92 150),(S 68 138))
  $g.FillPolygon((New-Object System.Drawing.SolidBrush (C '#CFE3F7')),$side); $g.DrawPolygon($tapeEdge,$side)
  # the seam of the flaps, on the other side of the tape
  $seam = New-Object System.Drawing.Pen (C '#7FB0E0'),([single]([Math]::Max(1,2.5*$k)))
  $g.DrawLine($seam,(S 176 56),(S 80 104))
  # RF on the right face, sheared with it
  $m = New-Object System.Drawing.Drawing2D.Matrix 1,-0.5,0,1,(128*$k),(128*$k)
  $saved = $g.Transform
  $g.MultiplyTransform($m)
  $fontName = 'Arial'
  if((New-Object System.Drawing.Text.InstalledFontCollection).Families.Name -contains 'Arial Black'){ $fontName = 'Arial Black' }
  $px = 46*$k
  $font = New-Object System.Drawing.Font $fontName,([single]$px),([System.Drawing.FontStyle]::Bold),([System.Drawing.GraphicsUnit]::Pixel)
  $fmt = New-Object System.Drawing.StringFormat
  $fmt.Alignment='Center'; $fmt.LineAlignment='Center'
  $box = New-Object System.Drawing.RectangleF 0,(34*$k),(96*$k),(60*$k)
  $shadowBox = New-Object System.Drawing.RectangleF (2.5*$k),(36*$k),(96*$k),(60*$k)
  $g.DrawString('RF',$font,(New-Object System.Drawing.SolidBrush (C '#16406E')),$shadowBox,$fmt)
  $g.DrawString('RF',$font,(New-Object System.Drawing.SolidBrush (C '#F6FAFF')),$box,$fmt)
  $g.Transform = $saved
  # the outline, and a light along the top edges
  $pen = New-Object System.Drawing.Pen $outline,([single]([Math]::Max(1.5,6*$k)))
  $pen.LineJoin='Round'
  $g.DrawPolygon($pen,[System.Drawing.PointF[]]@($N,$E,$E2,$S2,$W2,$W))
  $g.DrawLine($pen,$W,$Sx); $g.DrawLine($pen,$E,$Sx); $g.DrawLine($pen,$Sx,$S2)
  $hi = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150,255,255,255)),([single]([Math]::Max(1,3*$k)))
  $g.DrawLine($hi,(S 40 78),(S 128 40)); $g.DrawLine($hi,(S 128 40),(S 216 78))
  $g.Dispose()
  return $bmp
}
foreach($s in 48,64,128,256){
  $b = New-Vector $s
  $name = "$s-rf-packer.png"
  if($s -eq 48){ $name = '48-tmp.png' }
  $b.Save("$out\$name",[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}

# the .ico: PNG images of 16, 32, 48, 64, 128 and 256
$sizes = 16,32,48,64,128,256
$files = @{16="$out\16-rf-packer.png";32="$out\32-rf-packer.png";48="$out\48-tmp.png";64="$out\64-rf-packer.png";128="$out\128-rf-packer.png";256="$out\256-rf-packer.png"}
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16*$sizes.Count
$datas = @()
foreach($s in $sizes){ $d=[System.IO.File]::ReadAllBytes($files[$s]); $datas += ,$d
  $v = $s
  if($s -ge 256){ $v = 0 }
  $bw.Write([byte]$v); $bw.Write([byte]$v); $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$d.Length); $bw.Write([uint32]$offset); $offset += $d.Length }
foreach($d in $datas){ $bw.Write($d) }
[System.IO.File]::WriteAllBytes("$out\appicon.ico",$ms.ToArray())
Remove-Item "$out\48-tmp.png"
Get-ChildItem $out | Select-Object Name,Length
