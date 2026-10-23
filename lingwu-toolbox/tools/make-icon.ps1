# make-icon.ps1 -- generate resources/app.ico for lingwu-service-manager.
#
# Why a script instead of a committed binary blob: the icon is geometry, not
# artwork. Keeping the construction here means the .ico can be regenerated at
# any size, and the next person can see exactly what it is made of instead of
# squinting at a binary.
#
# Run with pwsh (not Windows PowerShell):
#   pwsh -NoProfile -File .\tools\make-icon.ps1
#   pwsh -NoProfile -File .\tools\make-icon.ps1 -Preview
#
# Design brief: it has to read as "the other LingwuLab tool" at a glance.
# lingwu-sysmon is a dark navy rounded square with an amber arc, so this one
# keeps the exact same plate and accent and swaps the mark for a cog -- which
# is also the metaphor Windows itself uses for services. Colours were sampled
# from the sysmon icon rather than guessed:
#   plate  #1E222B      accent #F5B942
#
# This file is pure ASCII (repo rule for .ps1).

[CmdletBinding()]
param(
    # Also drop a 256 px PNG next to the .ico so the mark can be eyeballed
    # without hunting for a file explorer.
    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$outIco = Join-Path $root 'resources\app.ico'

# Same size ladder lingwu-sysmon ships. 16 px is the one that actually has to
# survive, because that is what Explorer's small-icons view and the taskbar
# unpinned state both use.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

$plate  = [System.Drawing.Color]::FromArgb(255, 0x1E, 0x22, 0x2B)
$accent = [System.Drawing.Color]::FromArgb(255, 0xF5, 0xB9, 0x42)

function New-RoundedRectPath([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-Polar([double]$cx, [double]$cy, [double]$r, [double]$deg) {
    $rad = $deg * [Math]::PI / 180.0
    # -ArgumentList is explicit on purpose: `New-Object PointF($a,$b)` looks
    # equivalent but PowerShell's binder does not always route it as two
    # constructor args, and the failure shows up as an empty point rather than
    # an error.
    return New-Object System.Drawing.PointF -ArgumentList `
        ([float]($cx + $r * [Math]::Cos($rad))), ([float]($cy + $r * [Math]::Sin($rad)))
}

function New-GearPath([double]$cx, [double]$cy, [double]$rOuter, [double]$rRoot,
                      [int]$teeth) {
    # Built as an explicit polygon: for each tooth, two points out at rOuter
    # and two back at rRoot. A GraphicsPath with AddArc per tooth would leave
    # seams that show up as light pixels after downscaling.
    #
    # No centre hole here -- the hole is painted afterwards in the plate
    # colour. That is visually identical (the hole shows the plate anyway) and
    # avoids GraphicsPath.AddPath plus FillMode::Alternate, which is far more
    # machinery than a two-pixel circle deserves.
    $pts = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    $step = 360.0 / $teeth
    $toothHalf = $step * 0.19
    $shoulder = $step * 0.03
    for ($i = 0; $i -lt $teeth; $i++) {
        $a = $i * $step
        $pts.Add((New-Polar $cx $cy $rOuter ($a - $toothHalf)))
        $pts.Add((New-Polar $cx $cy $rOuter ($a + $toothHalf)))
        $pts.Add((New-Polar $cx $cy $rRoot  ($a + $toothHalf + $shoulder)))
        $pts.Add((New-Polar $cx $cy $rRoot  ($a + $step - $toothHalf - $shoulder)))
    }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddPolygon($pts.ToArray())
    return $path
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size,
                                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Plate. Corner radius is a fraction of the canvas so it looks identical at
    # every size -- WinUI's own app icon plate is roughly this round.
    $inset = $size * 0.02
    $plateRect = New-Object System.Drawing.RectangleF($inset, $inset,
                                                     ($size - 2 * $inset), ($size - 2 * $inset))
    $platePath = New-RoundedRectPath $plateRect.X $plateRect.Y $plateRect.Width `
                                    $plateRect.Height ($size * 0.225)
    $g.FillPath((New-Object System.Drawing.SolidBrush $plate), $platePath)
    $platePath.Dispose()

    # Cog. Proportions are tuned so the teeth survive a downscale to 16 px:
    # outer radius 0.34 of the canvas, root 0.265, hole 0.115, 8 teeth.
    $c = $size / 2.0
    $gearBrush = New-Object System.Drawing.SolidBrush -ArgumentList $accent
    $gear = New-GearPath $c $c ($size * 0.34) ($size * 0.265) 8
    $g.FillPath($gearBrush, $gear)
    $gearBrush.Dispose()
    $gear.Dispose()

    # Centre hole, painted in the plate colour so it reads as punched out.
    $rHole = $size * 0.115
    $holeBrush = New-Object System.Drawing.SolidBrush -ArgumentList $plate
    $g.FillEllipse($holeBrush, [float]($c - $rHole), [float]($c - $rHole),
                   [float]($rHole * 2), [float]($rHole * 2))
    $holeBrush.Dispose()

    $g.Dispose()
    return $bmp
}

function ConvertTo-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

# ---- render ---------------------------------------------------------------
$frames = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $frames += , [pscustomobject]@{ Size = $s; Bytes = (ConvertTo-PngBytes $bmp) }
    Write-Host ("  rendered {0,3} x {0,-3} ({1} bytes png)" -f $s, $frames[-1].Bytes.Length)
    if ($Preview -and $s -eq 256) {
        $bmp.Save((Join-Path $here 'app-icon-preview.png'),
                  [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bmp.Dispose()
}

# ---- pack into an .ico ----------------------------------------------------
# ICONDIR(6) + ICONDIRENTRY(16 each) + payloads. PNG payloads are fine on
# every Windows this project targets (10.0.17763+) and keep the file small;
# System.Drawing.Icon cannot write multi-size files at all, which is why this
# is hand-rolled.
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
$w.Write([uint16]0)          # reserved
$w.Write([uint16]1)          # type 1 = icon
$w.Write([uint16]$frames.Count)

$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }   # 0 means 256 in a .ico
    $w.Write([byte]$dim)
    $w.Write([byte]$dim)
    $w.Write([byte]0)         # colour count: 0 for >8bpp
    $w.Write([byte]0)         # reserved
    $w.Write([uint16]1)       # planes
    $w.Write([uint16]32)      # bit count
    $w.Write([uint32]$f.Bytes.Length)
    $w.Write([uint32]$offset)
    $offset += $f.Bytes.Length
}
foreach ($f in $frames) {
    $w.Write([byte[]]$f.Bytes)
}
$w.Flush()
$payload = $ms.ToArray()
$w.Dispose(); $ms.Dispose()

[System.IO.File]::WriteAllBytes($outIco, $payload)
Write-Host ("`n[OK] {0} = {1} bytes ({2} sizes)" -f `
            (Split-Path $outIco -Leaf), $payload.Length, $frames.Count)
