# make-icon.ps1 —— 生成零物工具箱的小蜜蜂图标（.ico + 应用内两处徽章）
#
# 用 pwsh 跑（不要用 Windows PowerShell）：
#   pwsh -NoProfile -File .\tools\build\make-icon.ps1
#   pwsh -NoProfile -File .\tools\build\make-icon.ps1 -Preview
#   pwsh -NoProfile -File .\tools\build\make-icon.ps1 -CompareSheet
#   pwsh -NoProfile -File .\tools\build\make-icon.ps1 -Source geometry
#   pwsh -NoProfile -File .\tools\build\make-icon.ps1 -Source art -Plate black -Coverage 0.88
#
# 【现在产出什么】
#   resources\app.ico            —— exe / 任务栏 / 托盘 / SFX 自解压启动器用的图标
#                                    （7 档：16/24/32/48/64/128/256，全 32bpp PNG 载荷）
#   resources\bee-badge-64.png   —— 应用内导航容器左端的徽章（28 DIP 位）
#   resources\bee-badge-32.png   —— 应用内标题栏文字前的徽章（16 DIP 位）
#
# 【造型来源：直接用参考图，不重绘】
# 2026-10-27 用户拍板：「不用绘制，直接把这张图片背景改为藏青背板，直接用」。
# 2026-10-28 换成新的「黑白可爱蜜蜂」参考图，底板随之改成近黑（-Plate black）。
# 所以主路线（-Source art，默认）是把 resources\bee-source.png 那张参考图
# **抠掉背景后原样合成到底板上** —— 一个像素都不重画。
# 原因很实际：三处（exe/托盘、标题栏、导航）要的是「同一只蜂」，
# 矢量重绘只能做到「像」，做不到「同一只」。
#
# 白底不能按亮度抠：白色在这张图里出现了两次 —— 背景，以及**眼睛里的高光**。
# 按亮度键控会把高光一起抠穿。抠图细节见 tools\build\render-bee-art.py 的注释。
#
# 【底板】
#   由 -Plate 决定：navy 藏青 #1E222B（品牌色）/ light 浅色 / black 近黑 #101114 / none 透明。
#   圆角方块：圆角半径 22.5%、内缩 2%。
#   .ico 与两张应用内徽章用的是**同一套底板参数**，所以三处看起来是同一枚图标。
#
# 【占比】
#   -Coverage 是蜂在底板里的占比，默认 0.80。往上调（0.88）小尺寸更清楚。
#
# 【候选对照】
#   换参考图或换底板时，先跑 tools\build\make-icon-sheet.py 出对照拼版给人看，
#   定下来再跑本脚本生成正式产物。对照拼版不碰 resources\app.ico。
#
# 【geometry 那条路还留着】
# -Source geometry 是**纯 PowerShell 的几何重绘**（不依赖 Python / Pillow），
# 蜂的每个部件都是可改的参数而不是一坨像素。它现在是备选，不是默认。
# 保留它的理由：本仓库有「产物可复现」的传统，纯几何路线没有外部依赖。
#
# 本文件是 UTF-8 无 BOM + LF。

[CmdletBinding()]
param(
    # 额外丢一张 256 px PNG 预览到 tools\build\app-icon-preview.png，
    # 免得为了看一眼图标还要去翻文件管理器。
    [switch]$Preview,

    # 图标来源：
    #   art      —— 用 resources\bee-source.png（参考图），抠掉背景后合成到指定底板。
    #               需要 python + Pillow（见 tools\build\render-bee-art.py）。
    #   geometry —— 用本文件里的矢量重绘（纯 PowerShell，无外部依赖），
    #               造型比参考图简单，小尺寸更锐利，但不是你给的那张参考图。
    [ValidateSet('art', 'geometry')]
    [string]$Source = 'art',

    # 仅 geometry 路径使用：底板与翅膀的配色方案。
    #   navy   藏青底板 + 蓝翅（默认）
    #   light  浅色底板 + 蓝翅
    #   whitewing 藏青底板 + 白翅
    #   noplate 无底板（透明背景）
    [ValidateSet('navy', 'light', 'whitewing', 'noplate')]
    [string]$Variant = 'navy',

    # 只生成候选对照拼版（build\icon-compare.png），**不碰** resources\app.ico。
    # 用来在定稿前把几个候选并排摊开给人看。
    [switch]$CompareSheet,

    # 参考图那条路（-Source art）用的底板与蜂的占比。
    # 默认值 = 当前已定稿的那一套（2026-10-28 用户选定「纯黑底板 · 占 88%」）。
    #   navy   藏青底板（品牌色）
    #   light  浅色底板
    #   black  近纯黑底板（默认）
    #   none   无底板（透明）
    [ValidateSet('navy', 'light', 'black', 'none')]
    [string]$Plate = 'black',

    # 蜂在底板里的占比。调大一点小尺寸更清楚。
    [ValidateRange(0.50, 1.0)]
    [double]$Coverage = 0.88,

    # 参考图那条路用的源图（默认 resources\bee-source.png）。
    [string]$ArtSource = '',

    # 浅色主题那张导航徽章用的源图（默认 resources\bee-source-light.png）。
    # 它是**另一张**参考图（黑蜂白底），不是白蜂那张的反色。
    [string]$LightSource = '',

    # 把每一档尺寸另外落成 PNG（对照拼版要逐档取用几何版本）。
    [string]$DumpDir = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

# 本文件在 tools/build/ 下，比工程根深两级 —— 与 tools/monitor/ 同深，
# 所以根目录要往上跳两次 Split-Path，不是一次。
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$outIco = Join-Path $root 'resources\app.ico'
$outPreview = Join-Path $here 'app-icon-preview.png'
$outSheet = Join-Path $root 'build\icon-compare.png'

# 随包发布的尺寸梯。16 px 是真正要活下来的那一档 ——
# 资源管理器的小图标视图、任务栏未固定状态、以及托盘的 SM_CXSMICON
# （本机 200% DPI ⇒ 托盘实际取的是 32 px 那档）都用它。
$sizes = @(16, 24, 32, 48, 64, 128, 256)

$plateNavy = [System.Drawing.Color]::FromArgb(255, 0x1E, 0x22, 0x2B)
$plateLight = [System.Drawing.Color]::FromArgb(255, 0xF3, 0xF4, 0xF6)
$bodyYellow = [System.Drawing.Color]::FromArgb(255, 0xF8, 0xC8, 0x18)
$darkBrown = [System.Drawing.Color]::FromArgb(255, 0x38, 0x20, 0x10)
$wingBlue = [System.Drawing.Color]::FromArgb(255, 0xC0, 0xE0, 0xF8)
$wingVein = [System.Drawing.Color]::FromArgb(255, 0xA0, 0xD0, 0xF0)
$wingWhite = [System.Drawing.Color]::FromArgb(255, 0xF2, 0xF4, 0xF7)
$white = [System.Drawing.Color]::FromArgb(255, 0xFF, 0xFF, 0xFF)

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
    # -ArgumentList 是显式写的：`New-Object PointF($a,$b)` 看着等价，
    # 但 PowerShell 的绑定器不保证把括号里的两个值当成构造函数参数，
    # 失败时表现为「拿到一个空点」而不是报错。
    return New-Object System.Drawing.PointF -ArgumentList `
        ([float]($cx + $r * [Math]::Cos($rad))), ([float]($cy + $r * [Math]::Sin($rad)))
}

function New-EllipsePath([double]$cx, [double]$cy, [double]$rx, [double]$ry) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddEllipse([float]($cx - $rx), [float]($cy - $ry), [float](2 * $rx), [float](2 * $ry))
    return $p
}

function New-GearPath([double]$cx, [double]$cy, [double]$rOuter, [double]$rRoot,
                      [int]$teeth) {
    # 显式多边形：每个齿两个点在 rOuter、两个点退回 rRoot。
    # 逐齿 AddArc 会在缩放后留下接缝亮线。
    #
    # 这里不挖中心孔 —— 孔稍后用底板色画出来，视觉完全一样，
    # 却省掉 GraphicsPath.AddPath + FillMode::Alternate 那套机制。
    $pts = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    $step = 360.0 / $teeth
    $toothHalf = $step * 0.19
    $shoulder = $step * 0.03
    for ($i = 0; $i -lt $teeth; $i++) {
        $a = $i * $step
        $pts.Add((New-Polar $cx $cy $rOuter ($a - $toothHalf)))
        $pts.Add((New-Polar $cx $cy $rOuter ($a + $toothHalf)))
        $pts.Add((New-Polar $cx $cy $rRoot ($a + $toothHalf + $shoulder)))
        $pts.Add((New-Polar $cx $cy $rRoot ($a + $step - $toothHalf - $shoulder)))
    }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddPolygon($pts.ToArray())
    return $path
}

# 一条圆头粗线（腿、翅脉都用它）。
# Round 端点是 GDI+ 的能力，XAML 的 Path 没有这个属性 —— 别把两边的写法搞混。
function New-RoundPen([System.Drawing.Color]$c, [double]$w) {
    $pen = New-Object System.Drawing.Pen -ArgumentList $c, ([float]$w)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    return $pen
}

# 把蜜蜂画进一张透明图层。
#   px     —— 图层边长（调用方给 4 倍超采样，最后缩回去，边缘才干净）
#   wing   —— 'blue' 浅蓝翅 / 'white' 白翅
#   detail —— 'full' 翅脉+笑脸+高光 / 'mid' 高光 / 'small' 只留眼睛
#
# 坐标全部是归一化的（0..1），照着参考图量出来的比例。
function New-BeeLayer([int]$px, [string]$wing, [string]$detail) {
    $bmp = New-Object System.Drawing.Bitmap($px, $px,
                                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # 【为什么要这层缩放】
    # 翅膀最高点在归一化坐标 v = -0.012、尖刺尖端在 u = 0.998，
    # 两处都超出图层边界 —— 直接画会被图层边缘切掉，
    # 表现是「翅膀上沿是一条平边」「尖刺短了一截」。
    # 所以整只蜂先缩到 0.96 再平移 0.02（先平移后缩放，顺序反了就不等式），
    # 让所有部件都落在图层内。
    #
    # 注意：翅膀和身体各自要做局部旋转，所以 ResetTransform 之后
    # 必须把这层底座变换重新装回去，否则后面画的部件会回到无变换状态。
    $g.TranslateTransform([float](0.02 * $px), [float](0.02 * $px))
    $g.ScaleTransform(0.96, 0.96)

    $wingColor = if ($wing -eq 'white') { $wingWhite } else { $wingBlue }
    $bodyBrush = New-Object System.Drawing.SolidBrush -ArgumentList $bodyYellow
    $darkBrush = New-Object System.Drawing.SolidBrush -ArgumentList $darkBrown
    $wingBrush = New-Object System.Drawing.SolidBrush -ArgumentList $wingColor
    $veinBrush = New-Object System.Drawing.SolidBrush -ArgumentList $wingVein
    $whiteBrush = New-Object System.Drawing.SolidBrush -ArgumentList $white

    # ---- 翅膀（在身体后面）--------------------------------------------
    # 两只翅膀都是「长轴斜指右上」的椭圆：前翅更陡（-55°）、后翅更平（-28°）。
    $wings = @(
        [pscustomobject]@{ CX = 0.5850; CY = 0.2200; A = 0.2700; B = 0.1250; Rot = -55 },
        [pscustomobject]@{ CX = 0.7650; CY = 0.2700; A = 0.2380; B = 0.1170; Rot = -28 }
    )
    foreach ($wg in $wings) {
        $cx = $wg.CX * $px
        $cy = $wg.CY * $px
        $g.TranslateTransform([float]$cx, [float]$cy)
        $g.RotateTransform([float]$wg.Rot)
        $g.FillEllipse($wingBrush, [float](-$wg.A * $px), [float](-$wg.B * $px),
                       [float](2 * $wg.A * $px), [float](2 * $wg.B * $px))

        if ($detail -eq 'full') {
            # 翅脉：顺着翅膀长轴的两道弧。缩到 64 px 以下就没意义了，
            # 它们在 32 px 上只会变成脏点。
            $veinPen = New-RoundPen $wingVein (0.038 * $px)
            $g.DrawBezier($veinPen,
                          [float](-$wg.A * 0.42 * $px), [float]($wg.B * 0.18 * $px),
                          [float](-$wg.A * 0.10 * $px), [float]($wg.B * 0.55 * $px),
                          [float]($wg.A * 0.30 * $px), [float]($wg.B * 0.30 * $px),
                          [float]($wg.A * 0.52 * $px), [float](-$wg.B * 0.10 * $px))
            $veinPen.Dispose()
        }
        $g.ResetTransform()
        $g.TranslateTransform([float](0.02 * $px), [float](0.02 * $px))
        $g.ScaleTransform(0.96, 0.96)
    }

    # ---- 身体 + 横纹 ---------------------------------------------------
    # 身体是个微微向右下倾的椭圆（+8°），横纹是竖着的带子，
    # 用椭圆轮廓做裁剪，带子两端就自然贴着身体边缘。
    $bCX = 0.6356
    $bCY = 0.6610
    $bRX = 0.2860
    $bRY = 0.1960
    $g.TranslateTransform([float]($bCX * $px), [float]($bCY * $px))
    $g.RotateTransform(8)
    $bodyPath = New-EllipsePath 0 0 ($bRX * $px) ($bRY * $px)
    $g.FillPath($bodyBrush, $bodyPath)
    $g.SetClip($bodyPath)
    # 横纹在身体局部坐标里的位置（相对身体中心，单位是归一化画布宽）。
    #
    # 【为什么要等宽交替】
    # 第一版按「深纹宽、浅纹窄」排，出来的效果是整个肚子几乎全黑，
    # 只剩几道细黄缝，看着像黄蜂不是蜜蜂。参考图是深浅几乎等宽地交替，
    # 黄色才是主体 —— 蜜蜂认的是「黄底 + 黑箍」，不是「黑底 + 黄箍」。
    #
    # 头的右缘在局部 -0.168，所以第一条深纹从那儿开始，
    # 视觉上就是「紧贴着头的第一个黑箍」。
    if ($detail -eq 'small') {
        # 16/24 px 上 4 条纹会糊成一片，只留两条更宽的。
        $bands = @(
            [pscustomobject]@{ X0 = -0.115; X1 = -0.015 },
            [pscustomobject]@{ X0 = 0.055; X1 = 0.155 }
        )
    } else {
        $bands = @(
            [pscustomobject]@{ X0 = -0.168; X1 = -0.103 },
            [pscustomobject]@{ X0 = -0.038; X1 = 0.027 },
            [pscustomobject]@{ X0 = 0.092; X1 = 0.157 },
            [pscustomobject]@{ X0 = 0.222; X1 = 0.286 }
        )
    }
    foreach ($b in $bands) {
        $g.FillRectangle($darkBrush,
                         [float]($b.X0 * $px), [float](-$bRY * $px),
                         [float](($b.X1 - $b.X0) * $px), [float](2 * $bRY * $px))
    }
    $g.ResetClip()
    $bodyPath.Dispose()
    $g.ResetTransform()
    $g.TranslateTransform([float](0.02 * $px), [float](0.02 * $px))
    $g.ScaleTransform(0.96, 0.96)

    # ---- 尖刺 -----------------------------------------------------------
    $stinger = New-Object System.Drawing.Drawing2D.GraphicsPath
    $stinger.AddPolygon([System.Drawing.PointF[]]@(
            (New-Object System.Drawing.PointF -ArgumentList ([float](0.8735 * $px)), ([float](0.6610 * $px))),
            (New-Object System.Drawing.PointF -ArgumentList ([float](0.9979 * $px)), ([float](0.7044 * $px))),
            (New-Object System.Drawing.PointF -ArgumentList ([float](0.8653 * $px)), ([float](0.7405 * $px)))
        ))
    $g.FillPath($darkBrush, $stinger)
    $stinger.Dispose()

    # ---- 三条腿 ---------------------------------------------------------
    # 腿是圆头粗线：前腿往外撇，中腿和后腿往下再往后。
    $legs = @(
        [pscustomobject]@{ X0 = 0.3919; Y0 = 0.7463; CX1 = 0.3711; CY1 = 0.7946; CX2 = 0.3473; CY2 = 0.8369; X1 = 0.3623; Y1 = 0.8755 },
        [pscustomobject]@{ X0 = 0.5191; Y0 = 0.7886; CX1 = 0.5212; CY1 = 0.8420; CX2 = 0.5403; CY2 = 0.8941; X1 = 0.5489; Y1 = 0.9394 },
        [pscustomobject]@{ X0 = 0.6250; Y0 = 0.7886; CX1 = 0.6377; CY1 = 0.8475; CX2 = 0.6780; CY2 = 0.8941; X1 = 0.6833; Y1 = 0.9364 }
    )
    $legPen = New-RoundPen $darkBrown (0.036 * $px)
    foreach ($l in $legs) {
        $g.DrawBezier($legPen,
                      [float]($l.X0 * $px), [float]($l.Y0 * $px),
                      [float]($l.CX1 * $px), [float]($l.CY1 * $px),
                      [float]($l.CX2 * $px), [float]($l.CY2 * $px),
                      [float]($l.X1 * $px), [float]($l.Y1 * $px))
    }
    $legPen.Dispose()

    # ---- 触角（在头后面，根部被头盖住）----------------------------------
    $antennae = @(
        [pscustomobject]@{ X0 = 0.2161; Y0 = 0.3646; CX1 = 0.2095; CY1 = 0.3015; CX2 = 0.1518; CY2 = 0.2614; X1 = 0.0381; Y1 = 0.2903; R = 0.0403 },
        [pscustomobject]@{ X0 = 0.3027; Y0 = 0.3559; CX1 = 0.3104; CY1 = 0.2880; CX2 = 0.2712; CY2 = 0.2080; X1 = 0.1674; Y1 = 0.2246; R = 0.0371 }
    )
    $antPen = New-RoundPen $darkBrown (0.037 * $px)
    foreach ($a in $antennae) {
        $g.DrawBezier($antPen,
                      [float]($a.X0 * $px), [float]($a.Y0 * $px),
                      [float]($a.CX1 * $px), [float]($a.CY1 * $px),
                      [float]($a.CX2 * $px), [float]($a.CY2 * $px),
                      [float]($a.X1 * $px), [float]($a.Y1 * $px))
        $g.FillEllipse($darkBrush,
                       [float](($a.X1 - $a.R) * $px), [float](($a.Y1 - $a.R) * $px),
                       [float](2 * $a.R * $px), [float](2 * $a.R * $px))
    }
    $antPen.Dispose()

    # ---- 头 -------------------------------------------------------------
    $hCX = 0.2651
    $hCY = 0.5530
    $hR = 0.1928
    $g.FillEllipse($bodyBrush,
                   [float](($hCX - $hR) * $px), [float](($hCY - $hR) * $px),
                   [float](2 * $hR * $px), [float](2 * $hR * $px))

    # 眼睛：竖长的椭圆，在脸的偏左位置
    $eCX = 0.2131
    $eCY = 0.5508
    $eRX = 0.0646
    $eRY = 0.0784
    $g.FillEllipse($darkBrush,
                   [float](($eCX - $eRX) * $px), [float](($eCY - $eRY) * $px),
                   [float](2 * $eRX * $px), [float](2 * $eRY * $px))
    if ($detail -ne 'small') {
        # 眼睛高光
        $g.FillEllipse($whiteBrush,
                       [float]((0.1854 - 0.0297) * $px), [float]((0.5000 - 0.0297) * $px),
                       [float](0.0594 * $px), [float](0.0594 * $px))
    }
    if ($detail -eq 'full') {
        # 笑脸：脸在左边（头在左、身子在右），所以嘴在头的左下方
        $smilePen = New-RoundPen $darkBrown (0.019 * $px)
        $g.DrawBezier($smilePen,
                      [float](0.0985 * $px), [float](0.6506 * $px),
                      [float](0.1050 * $px), [float](0.6860 * $px),
                      [float](0.1300 * $px), [float](0.6880 * $px),
                      [float](0.1462 * $px), [float](0.6733 * $px))
        $smilePen.Dispose()
    }

    $bodyBrush.Dispose()
    $darkBrush.Dispose()
    $wingBrush.Dispose()
    $veinBrush.Dispose()
    $whiteBrush.Dispose()
    $g.Dispose()
    return $bmp
}

# 画一整张图标（底板 + 蜂）。
# $variant: 'gear'（对照拼版的现状列）/ 'navy' / 'light' / 'whitewing' / 'noplate'
function New-IconBitmap([int]$size, [string]$variant) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size,
                                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $plateBrush = $null
    if ($variant -eq 'gear') {
        # 现状对照列：旧齿轮图标，参数与旧脚本一致，方便直接比。
        $plateBrush = New-Object System.Drawing.SolidBrush -ArgumentList $plateNavy
        $inset = $size * 0.02
        $plateRect = New-Object System.Drawing.RectangleF($inset, $inset,
                                                          ($size - 2 * $inset), ($size - 2 * $inset))
        $platePath = New-RoundedRectPath $plateRect.X $plateRect.Y $plateRect.Width `
                                        $plateRect.Height ($size * 0.225)
        $g.FillPath($plateBrush, $platePath)
        $platePath.Dispose()

        $c = $size / 2.0
        $gearBrush = New-Object System.Drawing.SolidBrush -ArgumentList $bodyYellow
        $gear = New-GearPath $c $c ($size * 0.34) ($size * 0.265) 8
        $g.FillPath($gearBrush, $gear)
        $gearBrush.Dispose()
        $gear.Dispose()
        $rHole = $size * 0.115
        $g.FillEllipse($plateBrush, [float]($c - $rHole), [float]($c - $rHole),
                       [float]($rHole * 2), [float]($rHole * 2))
        $plateBrush.Dispose()
        $g.Dispose()
        return $bmp
    }

    # ---- 底板 ----
    $plateColor = switch ($variant) {
        'light' { $plateLight }
        'noplate' { $null }
        default { $plateNavy }
    }
    if ($null -ne $plateColor) {
        $plateBrush = New-Object System.Drawing.SolidBrush -ArgumentList $plateColor
        $inset = $size * 0.02
        $plateRect = New-Object System.Drawing.RectangleF($inset, $inset,
                                                          ($size - 2 * $inset), ($size - 2 * $inset))
        $platePath = New-RoundedRectPath $plateRect.X $plateRect.Y $plateRect.Width `
                                        $plateRect.Height ($size * 0.225)
        $g.FillPath($plateBrush, $platePath)
        $platePath.Dispose()
    }

    # ---- 蜂 ----
    $detail = if ($size -ge 64) { 'full' } elseif ($size -ge 32) { 'mid' } else { 'small' }
    $wing = if ($variant -eq 'whitewing') { 'white' } else { 'blue' }

    $layer = New-BeeLayer ($size * 4) $wing $detail
    # 蜂的外框在图层里占 0.96 宽（天线的球头到尖刺），
    # 画布取 0.84 倍 ⇒ 蜂身约占底板 0.81 宽，四个角都不会顶出圆角底板。
    $t = $size * 0.84
    $g.DrawImage($layer, [float](($size - $t) / 2), [float](($size - $t) / 2),
                 [float]$t, [float]$t)
    $layer.Dispose()

    if ($null -ne $plateBrush) { $plateBrush.Dispose() }
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

function Get-SheetFont([single]$size, [System.Drawing.FontStyle]$style) {
    $installed = (New-Object System.Drawing.Text.InstalledFontCollection).Families
    $name = $null
    foreach ($cand in @('Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI')) {
        if ($installed | Where-Object { $_.Name -eq $cand }) { $name = $cand; break }
    }
    if (-not $name) { $name = 'Segoe UI' }
    return New-Object System.Drawing.Font -ArgumentList $name, $size, $style
}

# 候选对照拼版：现状列 + 4 个候选列，每列给出 256/64/32/16 真实尺寸，
# 外加 16 px x8、32 px x4 的最近邻放大 —— 放大用最近邻而不是双线性，
# 是为了让小尺寸下真实的锯齿/糊点原形毕露（双线性会把问题抹平掉）。
function New-CompareSheet([string]$outPath) {
    $cols = @(
        [pscustomobject]@{ Title = '现状：齿轮'; Variant = 'gear'; Note = '当前图标（对照基线）' },
        [pscustomobject]@{ Title = '藏青底板 + 蓝翅'; Variant = 'navy'; Note = '保留品牌底板' },
        [pscustomobject]@{ Title = '浅色底板 + 蓝翅'; Variant = 'light'; Note = '深色线条更清楚' },
        [pscustomobject]@{ Title = '藏青底板 + 白翅'; Variant = 'whitewing'; Note = '去掉蓝色，只剩黑黄' },
        [pscustomobject]@{ Title = '无底板（透明）'; Variant = 'noplate'; Note = '跟随任务栏底色' }
    )
    $rows = @(
        [pscustomobject]@{ Size = 256; Zoom = 1; Label = '256 px' },
        [pscustomobject]@{ Size = 64; Zoom = 1; Label = '64 px' },
        [pscustomobject]@{ Size = 32; Zoom = 1; Label = '32 px' },
        [pscustomobject]@{ Size = 16; Zoom = 1; Label = '16 px' },
        [pscustomobject]@{ Size = 16; Zoom = 8; Label = '16 px x8 最近邻' },
        [pscustomobject]@{ Size = 32; Zoom = 4; Label = '32 px x4 最近邻' }
    )

    $cell = 256
    $gutter = 104
    $padX = 18
    $padY = 20
    $headH = 96
    $gap = 14
    $footH = 56

    $rowHeights = @()
    foreach ($r in $rows) {
        $h = [int]($r.Size * $r.Zoom)
        if ($h -lt 46) { $h = 46 }
        $rowHeights += $h
    }
    $bodyH = ($rowHeights | Measure-Object -Sum).Sum + $gap * ($rows.Count + 1)
    $cardTop = $padY + $headH
    $cardH = $bodyH + 10
    $width = $gutter + $padX + $cols.Count * ($cell + $padX)
    $height = $cardTop + $cardH + $padY + $footH

    $sheet = New-Object System.Drawing.Bitmap($width, $height,
                                               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $g.Clear([System.Drawing.Color]::FromArgb(255, 0xEE, 0xF0, 0xF4))

    $fontTitle = Get-SheetFont 21 ([System.Drawing.FontStyle]::Bold)
    $fontCol = Get-SheetFont 15 ([System.Drawing.FontStyle]::Bold)
    $fontNote = Get-SheetFont 12 ([System.Drawing.FontStyle]::Regular)
    $fontRow = Get-SheetFont 12 ([System.Drawing.FontStyle]::Regular)
    $dark = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 0x24, 0x28, 0x30))
    $grey = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 0x6B, 0x72, 0x7D))
    $accent = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 0xC0, 0x7A, 0x12))

    $g.DrawString('小蜜蜂图标候选对照（参照你给的卡通蜜蜂重画）', $fontTitle, $dark, [float]$padX, [float]$padY)
    $g.DrawString('身体黄 #F8C818 · 深褐 #382010 · 放大行是最近邻，看的是真实像素',
                  $fontNote, $grey, [float]$padX, [float]($padY + 30))

    # 每列一张白卡
    for ($c = 0; $c -lt $cols.Count; $c++) {
        $x = $gutter + $padX + $c * ($cell + $padX)
        $card = New-Object System.Drawing.RectangleF([float]$x, [float]$cardTop,
                                                     [float]$cell, [float]$cardH)
        $cardPath = New-RoundedRectPath $card.X $card.Y $card.Width $card.Height 14
        $cardBrush = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::FromArgb(255, 0xFF, 0xFF, 0xFF))
        $g.FillPath($cardBrush, $cardPath)
        $borderPen = New-Object System.Drawing.Pen -ArgumentList ([System.Drawing.Color]::FromArgb(255, 0xDC, 0xE0, 0xE6)), 1
        $g.DrawPath($borderPen, $cardPath)
        $borderPen.Dispose()
        $cardBrush.Dispose()
        $cardPath.Dispose()

        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = [System.Drawing.StringAlignment]::Center
        $titleRect = New-Object System.Drawing.RectangleF([float]$x, [float]($cardTop - 42),
                                                         [float]$cell, [float]22)
        $g.DrawString($cols[$c].Title, $fontCol, $dark, $titleRect, $fmt)
        $noteRect = New-Object System.Drawing.RectangleF([float]$x, [float]($cardTop - 21),
                                                        [float]$cell, [float]18)
        $g.DrawString($cols[$c].Note, $fontNote, $grey, $noteRect, $fmt)
    }

    # 行内容
    $y = $cardTop + 8
    for ($r = 0; $r -lt $rows.Count; $r++) {
        $row = $rows[$r]
        $rh = $rowHeights[$r]
        $labelRect = New-Object System.Drawing.RectangleF([float]($padX - 6), [float]($y + ($rh / 2) - 12),
                                                         [float]($gutter - $padX - 8), [float]24)
        $fmtLeft = New-Object System.Drawing.StringFormat
        $fmtLeft.Alignment = [System.Drawing.StringAlignment]::Far
        $g.DrawString($row.Label, $fontRow, $grey, $labelRect, $fmtLeft)

        for ($c = 0; $c -lt $cols.Count; $c++) {
            $x = $gutter + $padX + $c * ($cell + $padX)
            $icon = New-IconBitmap $row.Size $cols[$c].Variant
            $drawW = $row.Size * $row.Zoom
            $dx = $x + ($cell - $drawW) / 2
            $dy = $y + ($rh - $drawW) / 2
            if ($row.Zoom -gt 1) {
                # 最近邻放大：像素块要方，不能被插值抹圆。
                $oldInterp = $g.InterpolationMode
                $oldOffset = $g.PixelOffsetMode
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
                $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
                $g.DrawImage($icon, [float]$dx, [float]$dy, [float]$drawW, [float]$drawW)
                $g.InterpolationMode = $oldInterp
                $g.PixelOffsetMode = $oldOffset
            } else {
                $g.DrawImage($icon, [float]$dx, [float]$dy)
            }
            $icon.Dispose()
        }
        $y += $rh + $gap
    }

    $g.DrawString('看图选一列：哪一列一眼能看出是只蜜蜂，就选哪一列。',
                  $fontNote, $accent, [float]$padX, [float]($cardTop + $cardH + 10))

    $fontTitle.Dispose()
    $fontCol.Dispose()
    $fontNote.Dispose()
    $fontRow.Dispose()
    $dark.Dispose()
    $grey.Dispose()
    $accent.Dispose()
    $g.Dispose()

    $dir = Split-Path -Parent $outPath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $sheet.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $sheet.Dispose()
}

# ---- 对照拼版模式：只出图，不碰 resources\app.ico -------------------------
# 定稿前必须先给人看图，所以这一路刻意不去覆盖真正的图标文件。
if ($CompareSheet) {
    Write-Host 'rendering comparison sheet ...'
    New-CompareSheet $outSheet
    Write-Host ("[OK] {0} = {1} bytes" -f (Split-Path $outSheet -Leaf), (Get-Item $outSheet).Length)
    return
}

# ---- render ---------------------------------------------------------------
$frames = @()

if ($Source -eq 'art') {
    # 参考图那条路：python 负责抠图与合成（位图处理用 numpy 更快也更准），
    # 产出的各档 PNG 再走下面同一段打包代码 —— 打包格式两条产线是一致的。
    $artScript = Join-Path $here 'render-bee-art.py'
    $artSource = if ($ArtSource) { $ArtSource } else { Join-Path $root 'resources\bee-source.png' }
    $artDir = Join-Path $root (Join-Path 'build\bee-art' $Plate)
    if (-not (Test-Path $artScript)) { throw "tools\build\render-bee-art.py not found." }
    if (-not (Test-Path $artSource)) { throw "$artSource not found." }
    & python $artScript --source $artSource --outdir $artDir --plate $Plate --coverage $Coverage
    if ($LASTEXITCODE -ne 0) { throw "render-bee-art.py failed (exit $LASTEXITCODE)." }
    foreach ($s in $sizes) {
        $png = Join-Path $artDir ("{0}-{1}.png" -f $Plate, $s)
        if (-not (Test-Path $png)) { throw "render-bee-art.py did not produce $png" }
        $frames += , [pscustomobject]@{ Size = $s; Bytes = [System.IO.File]::ReadAllBytes($png) }
        Write-Host ("  packed   {0,3} x {0,-3} ({1} bytes png)" -f $s, $frames[-1].Bytes.Length)
        if ($Preview -and $s -eq 256) {
            [System.IO.File]::Copy($png, $outPreview, $true)
        }
        if ($DumpDir) {
            if (-not (Test-Path $DumpDir)) { New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null }
            [System.IO.File]::Copy($png, (Join-Path $DumpDir ("{0}.png" -f $s)), $true)
        }
    }

    # 应用内两处标记用的徽章（位图，直接引用，不重绘）：
    #
    #   bee-badge-32.png       —— 标题栏文字前的图标位（16 DIP，200% DPI 下 32 物理像素）
    #                            底板与 .ico 一致（带板），两种主题下都成立：
    #                            底板本身提供了对比度。
    #   bee-badge-64.png       —— 导航容器左端的 logo 位（28 DIP，200% DPI 下 56 物理像素）
    #                            **透明底**（无底板）：导航容器本身已经是一个药丸形容器，
    #                            再压一块近黑方底会像「挖了个洞」—— 2026-10-28 用户拍板去掉。
    #   bee-badge-64-light.png —— 同上，但蜂是**黑色**的（浅色主题用）。
    #
    # 【为什么导航徽章要出两版】
    # 蜂的原图是「白蜂 + 黑条纹」。透明底之后，白色那一坨在浅色主题的
    # 药丸容器（≈ #F0F0F0）上等于隐形。所以浅色主题必须换成黑蜂，
    # 深色主题用白蜂 —— 由 App.xaml 的 ThemeDictionaries 按主题选图。
    #
    # exe / 托盘图标仍然是近黑底板 + 白蜂：它在任务栏、托盘、资源管理器里
    # 各自有各的底色，透明底在那几处会随系统主题变来变去，反而不可控。
    $badgeTargets = @()
    if ($Plate -ne 'none') {
        $badgeTargets += , @{ From = ('{0}-32.png' -f $Plate); To = 'bee-badge-32.png' }
    }

    # 透明底的两版导航徽章：白蜂（深色主题）与黑蜂（浅色主题）
    $noneDir = Join-Path $root (Join-Path 'build\bee-art' 'none')
    & python $artScript --source $artSource --outdir $noneDir --plate none --coverage $Coverage
    if ($LASTEXITCODE -ne 0) { throw "render-bee-art.py (transparent) failed (exit $LASTEXITCODE)." }

    # 浅色主题那张用的是**另一张源图**（resources\bee-source-light.png，黑蜂白底），
    # 不是把白蜂反色 —— 2026-10-28 用户给的是一张造型不同的卡通蜜蜂
    # （头更大、触角更粗、笑脸不同，与白蜂那张的剪影 IoU 只有 0.66）。
    # 反色那套（--invert）还在 render-bee-art.py 里备用，但正式产物不走它。
    $lightSource = if ($LightSource) { $LightSource } else { Join-Path $root 'resources\bee-source-light.png' }
    $lightDir = Join-Path $root (Join-Path 'build\bee-art' 'light')
    if (-not (Test-Path $lightSource)) { throw "$lightSource not found." }
    & python $artScript --source $lightSource --outdir $lightDir --plate none --coverage $Coverage
    if ($LASTEXITCODE -ne 0) { throw "render-bee-art.py (light source) failed (exit $LASTEXITCODE)." }

    $badgeTargets += , @{ From = 'none-64.png'; To = 'bee-badge-64.png';       Dir = $noneDir }
    $badgeTargets += , @{ From = 'none-64.png'; To = 'bee-badge-64-light.png'; Dir = $lightDir }

    foreach ($b in $badgeTargets) {
        $dir = if ($b.ContainsKey('Dir')) { $b.Dir } else { $artDir }
        $from = Join-Path $dir $b.From
        if (-not (Test-Path $from)) { throw "render-bee-art.py did not produce $from" }
        $to = Join-Path $root (Join-Path 'resources' $b.To)
        [System.IO.File]::Copy($from, $to, $true)
        Write-Host ("  badge    {0} <- {1}" -f $b.To, $b.From)
    }
} else {
    foreach ($s in $sizes) {
        $bmp = New-IconBitmap $s $Variant
        $frames += , [pscustomobject]@{ Size = $s; Bytes = (ConvertTo-PngBytes $bmp) }
        Write-Host ("  rendered {0,3} x {0,-3} ({1} bytes png)" -f $s, $frames[-1].Bytes.Length)
        if ($Preview -and $s -eq 256) {
            $bmp.Save($outPreview, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        if ($DumpDir) {
            if (-not (Test-Path $DumpDir)) { New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null }
            $bmp.Save((Join-Path $DumpDir ("{0}.png" -f $s)), [System.Drawing.Imaging.ImageFormat]::Png)
        }
        $bmp.Dispose()
    }
}

# ---- pack into an .ico ----------------------------------------------------
# ICONDIR(6) + ICONDIRENTRY(16 each) + payloads。PNG payloads 在本项目支持的
# 每一版 Windows（10.0.17763+）上都没问题，而且文件小；
# System.Drawing.Icon 压根不支持多尺寸写入 —— 这也是这里手写打包的原因。
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
Write-Host ("`n[OK] {0} = {1} bytes ({2} sizes, source = {3})" -f `
            (Split-Path $outIco -Leaf), $payload.Length, $frames.Count, $Source)
