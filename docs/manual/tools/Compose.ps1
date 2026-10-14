# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

# Compose.ps1 — build the manual's annotated / composite images from the raw
# captures. Re-runnable: it only reads images/ and writes back into it.

Add-Type -AssemblyName System.Drawing
$IMG = Join-Path $PSScriptRoot "..\images"

function Load($n) { [System.Drawing.Image]::FromFile((Join-Path $IMG $n)) }
function Save($bmp,$n) {
    $bmp.Save((Join-Path $IMG $n), [System.Drawing.Imaging.ImageFormat]::Png)
    "$n  $($bmp.Width)x$($bmp.Height)"
}

$accent = [System.Drawing.Color]::FromArgb(255, 255, 160, 25)   # the editor's own orange
$chipBg = [System.Drawing.Color]::FromArgb(235, 20, 22, 26)
$white  = [System.Drawing.Color]::White

function NewG($bmp) {
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'ClearTypeGridFit'
    $g.InterpolationMode = 'HighQualityBicubic'
    return $g
}

# A label chip: dark rounded background + accent text, so it reads over anything.
function Chip($g,$text,$x,$y,$fontSize=13,$anchorRight=$false) {
    $f  = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold)
    $sz = $g.MeasureString($text, $f)
    $w  = [int]$sz.Width + 12; $h = [int]$sz.Height + 6
    if ($anchorRight) { $x = $x - $w }
    $g.FillRectangle((New-Object System.Drawing.SolidBrush($chipBg)), $x, $y, $w, $h)
    $g.DrawRectangle((New-Object System.Drawing.Pen($accent, 1)), $x, $y, $w, $h)
    $g.DrawString($text, $f, (New-Object System.Drawing.SolidBrush($accent)), ($x+6), ($y+3))
    return @($x, $y, $w, $h)
}

# ---------------------------------------------------------------- 1. modules map
function Build-Modules {
    $src = Load "first-run-editor.png"
    $bmp = New-Object System.Drawing.Bitmap($src.Width, $src.Height)
    $g = NewG $bmp
    $g.DrawImage($src, 0, 0, $src.Width, $src.Height)
    # dim the whole thing slightly so the outlines carry
    $g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(90,0,0,0))), 0,0,$bmp.Width,$bmp.Height)
    $pen = New-Object System.Drawing.Pen($accent, 2)
    # Seven of the nine modules already print their own name on screen, so a
    # chip there would only cover a control. Outline all nine; label just the
    # two that carry no title, and put those chips on empty pixels.
    $mods = @(
        @{n=$null;             r=@(82,36,500,156)   },  # System
        @{n=$null;             r=@(82,162,500,286)  },  # Multi EQ
        @{n="Graphic Display"; r=@(504,38,1222,286);  lx=560;  ly=96  },
        @{n=$null;             r=@(82,296,500,412)  },  # Effect
        @{n=$null;             r=@(506,296,1224,412)},  # Controller
        @{n=$null;             r=@(82,420,1222,534) },  # Effect Parameters
        @{n=$null;             r=@(82,540,1222,813) },  # Part
        @{n=$null;             r=@(1228,36,1558,793)},  # Voice
        @{n="Keyboard";        r=@(82,814,1558,953);  lx=1350; ly=822 }
    )
    foreach ($m in $mods) {
        $r = $m.r
        $g.DrawRectangle($pen, $r[0], $r[1], ($r[2]-$r[0]), ($r[3]-$r[1]))
        if ($m.n) { [void](Chip $g $m.n $m.lx $m.ly 13) }
    }
    $out = Save $bmp "editor-modules.png"
    $g.Dispose(); $bmp.Dispose(); $src.Dispose(); $out
}

# ---------------------------------------------------------------- 2. two views
function Build-TwoViews {
    $hw = Load "first-run-hardware.png"
    $ed = Load "first-run-editor.png"
    $W  = 1600
    $hwH = [int]($hw.Height * $W / $hw.Width)
    $edH = [int]($ed.Height * $W / $ed.Width)
    $gap = 34
    $bmp = New-Object System.Drawing.Bitmap($W, ($hwH + $gap + $edH))
    $g = NewG $bmp
    $g.Clear([System.Drawing.Color]::FromArgb(255,22,24,28))
    $g.DrawImage($hw, 0, 0, $W, $hwH)
    $g.DrawImage($ed, 0, ($hwH + $gap), $W, $edH)
    [void](Chip $g "Hardware view" 10 10 14)
    [void](Chip $g "Editor view"   10 ($hwH + $gap + 10) 14)
    $out = Save $bmp "two-views.png"
    $g.Dispose(); $bmp.Dispose(); $hw.Dispose(); $ed.Dispose(); $out
}

# ---------------------------------------------------------------- 3. bottom bar
function Build-BottomBar {
    $src = Load "bottom-bar.png"
    $topH = 74
    $bmp = New-Object System.Drawing.Bitmap($src.Width, ($src.Height + $topH))
    $g = NewG $bmp
    $g.Clear([System.Drawing.Color]::FromArgb(255,22,24,28))
    $g.DrawImage($src, 0, $topH, $src.Width, $src.Height)
    $pen = New-Object System.Drawing.Pen($accent, 2)
    # x centres of each control on the bar, and what to call it
    $marks = @(
        @{x=53;   n="File"},
        @{x=151;  n="Options"},
        @{x=249;  n="About"},
        @{x=347;  n="PANIC"},
        @{x=441;  n="Undo / HISTORY / Redo"},
        @{x=760;  n="Loaded file  ( * = unsaved )"}
    )
    $row = 0
    foreach ($m in $marks) {
        $ly = 6 + ($row % 2) * 32
        $c = Chip $g $m.n ($m.x) $ly 12
        # tick from the chip down to the bar
        $g.DrawLine($pen, $m.x, ($ly + $c[3]), $m.x, ($topH + 4))
        $row++
    }
    $out = Save $bmp "bottom-bar-annotated.png"
    $g.Dispose(); $bmp.Dispose(); $src.Dispose(); $out
}

# ---------------------------------------------------------------- 4. control types
function Build-ControlTypes {
    $src = Load "first-run-editor.png"
    # crop rects in the 1636x1000 editor, with the name to print under each.
    # All five sit in modules LEFT of the Voice column, so the 2026-08-30 canvas
    # widening did not move them -- see tools/README.md on the window size.
    $items = @(
        @{n="Knob";          r=@(92,704,164,788)},
        # The EQ grid's FREQ cell. Its Y was re-derived on 2026-09-11, when the EQ
        # block moved up one band-label line and its rows grew by the override bar's
        # strip -- a crop into a MODULE's interior is only valid for the layout it
        # was measured on, however stable the module's own rect is.
        @{n="Value box";     r=@(840,589,908,610)},
        @{n="Selection box"; r=@(92,426,240,446)},
        @{n="LEDs";          r=@(90,791,212,811)},
        @{n="Controller wheel"; r=@(518,316,548,402)}
    )
    $cellW = 250; $cellH = 150; $labelH = 30
    $bmp = New-Object System.Drawing.Bitmap(($cellW * $items.Count), ($cellH + $labelH))
    $g = NewG $bmp
    $g.Clear([System.Drawing.Color]::FromArgb(255,26,29,33))
    $f = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'
    $i = 0
    foreach ($it in $items) {
        $r = $it.r
        $cw = $r[2]-$r[0]; $ch = $r[3]-$r[1]
        # scale up to fill the cell, capped so nothing gets silly
        $scale = [Math]::Min(3.2, [Math]::Min(($cellW-40)/[double]$cw, ($cellH-30)/[double]$ch))
        $dw = [int]($cw*$scale); $dh = [int]($ch*$scale)
        $dx = $i*$cellW + [int](($cellW-$dw)/2)
        $dy = [int](($cellH-$dh)/2)
        $dest = New-Object System.Drawing.Rectangle($dx,$dy,$dw,$dh)
        $srcR = New-Object System.Drawing.Rectangle($r[0],$r[1],$cw,$ch)
        $g.DrawImage($src, $dest, $srcR, [System.Drawing.GraphicsUnit]::Pixel)
        $g.DrawRectangle((New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(120,255,255,255),1)), $dest)
        $rect = New-Object System.Drawing.RectangleF(($i*$cellW), $cellH, $cellW, $labelH)
        $g.DrawString($it.n, $f, (New-Object System.Drawing.SolidBrush($accent)), $rect, $fmt)
        $i++
    }
    $out = Save $bmp "control-types.png"
    $g.Dispose(); $bmp.Dispose(); $src.Dispose(); $out
}

Build-Modules
Build-TwoViews
Build-BottomBar
Build-ControlTypes
