# tray.ps1 -- locating and driving the toolbox's tray icon through UI
# Automation.
#
# Dot-source this AFTER common.ps1. It needs the UIAutomation assemblies,
# which the calling script usually loads itself; this file only adds them if
# they are not there yet.
#
# WHY THIS IS NOT A ONE-LINER
#
#   Win11 keeps tray icons in two places:
#     * the visible area, under the taskbar window (Shell_TrayWnd), and
#     * the "^" overflow flyout, a SEPARATE top-level window
#       (TopLevelWindowForOverflowXamlIsland) that exists ONLY while the
#       flyout is open.
#
#   Which one an icon lands in is not up to the app. Win11 puts every new
#   icon in the overflow, and the app can only ask Explorer to promote it
#   with an undocumented registry value (see platform::TryPromoteTrayIcon).
#   That promotion is best-effort -- measured on this machine, Explorer does
#   not even create the registry entry the trick depends on -- so an
#   acceptance script that only looks at Shell_TrayWnd reports "no tray
#   icon" on a machine that very much has one.
#
#   The name Explorer shows is not stable either: it is the tooltip that
#   was current when the icon was FIRST registered for that (exe path, uid)
#   pair, and Explorer never refreshes it, so it can be an old label
#   ("Lingwu SysMon ..."). Match the product prefix, not the exact tip.
#
#   THE ELEMENT IS ONLY USABLE WHILE THE FLYOUT IS OPEN.
#   Once the flyout is closed, the element it handed out is stale: reading
#   its BoundingRectangle comes back as NaN / Infinity, and casting that to
#   an int throws "无法将值 NaN 转换为类型 System.Int32".
#   So the two entry points below come in pairs: Get-ToolboxTrayButton
#   leaves the flyout open (the caller must close it with
#   Close-ToolboxTray), while Test-ToolboxTrayIconPresent and
#   Invoke-ToolboxTrayButton open it, use it and close it again.
#
#   The overflow flyout can be opened AND closed through UI Automation
#   (Invoke the "show hidden icons" chevron twice), measured 2026-10-23:
#   invoking it opens the flyout and the toolbox icon shows up with the
#   current tooltip; invoking it again closes the flyout and the window
#   disappears.
#
# This file contains ONE Chinese literal (the chevron's label) and therefore
# has to be run with pwsh, like every other .ps1 in this repo that has CJK in
# it. The rest of the file is ASCII.

if (-not ('System.Windows.Automation.AutomationElement' -as [type])) {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
}

$script:TrayAE = [System.Windows.Automation.AutomationElement]
$script:TrayTS = [System.Windows.Automation.TreeScope]
$script:TrayCT = [System.Windows.Automation.ControlType]

# The InvokePattern INSTANCE, not the type.
#
# GetCurrentPattern takes an AutomationPattern object. `::Pattern` is that
# object; the bare type `[InvokePattern]` is a RuntimeType and passing it
# makes GetCurrentPattern throw
# 「无法将类型 System.Windows.Automation.InvokePattern 转换为类型
#   System.Windows.Automation.AutomationPattern」
# which -- with $ErrorActionPreference = 'Stop' -- takes the whole script
# down and blames the wrong thing entirely.
$script:TrayIP = [System.Windows.Automation.InvokePattern]::Pattern

# Invoke an element through its InvokePattern.
#
# Returns $true when the pattern was found and invoked, $false when the
# element does not support it. "Not supported" is a normal answer here: the
# shell's own buttons differ between Windows builds.
function Invoke-UiaInvoke($El) {
    if (-not $El) { return $false }
    try {
        $pat = $El.GetCurrentPattern($script:TrayIP)
    } catch { return $false }
    if (-not $pat) { return $false }
    try {
        $pat.Invoke()
    } catch { return $false }
    return $true
}

# Does this element support InvokePattern?
function Test-UiaInvokeSupported($El) {
    if (-not $El) { return $false }
    try {
        $null = $El.GetCurrentPattern($script:TrayIP)
        return $true
    } catch { return $false }
}

# The taskbar window that holds the visible tray icons.
function Get-TrayTaskbar {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $script:TrayAE::ClassNameProperty, 'Shell_TrayWnd')
    return $script:TrayAE::RootElement.FindFirst($script:TrayTS::Children, $cond)
}

# The overflow flyout window, or $null when it is not open.
function Get-TrayOverflowWindow {
    $wins = $script:TrayAE::RootElement.FindAll(
        $script:TrayTS::Children,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($w in $wins) {
        if ($w.Current.ClassName -match 'Overflow') { return $w }
    }
    return $null
}

# The "show hidden icons" chevron on the taskbar.
#
# How to recognise it: the chevron and the system buttons (network, volume,
# power) share the AutomationId "SystemTrayIcon", so the id does not tell
# them apart -- only the label does, and the label is localised. So the
# label is tried first (Chinese and English), and if neither matches we
# fall back to the FIRST "SystemTrayIcon" button in the tree, which is the
# chevron on this machine (measured: the chevron is followed by the system
# buttons, and the app icons carry "NotifyItemIcon" instead).
function Get-TrayChevron {
    $tray = Get-TrayTaskbar
    if (-not $tray) { return $null }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $script:TrayAE::ControlTypeProperty, $script:TrayCT::Button)
    $buttons = $tray.FindAll($script:TrayTS::Descendants, $cond)

    $fallback = $null
    foreach ($b in $buttons) {
        if ($b.Current.ClassName -ne 'SystemTray.NormalButton') { continue }
        if ($b.Current.Name -match '显示隐藏|hidden|overflow') { return $b }
        if (-not $fallback -and $b.Current.AutomationId -eq 'SystemTrayIcon') {
            $fallback = $b
        }
    }
    return $fallback
}

function Invoke-TrayChevron {
    $chev = Get-TrayChevron
    if (-not $chev) { return $false }
    return (Invoke-UiaInvoke $chev)
}

# The toolbox's own icon inside one window, matched by product prefix. The
# name Explorer shows is whatever tooltip was current at first registration,
# which may be an old one, so the exact text is NOT a reliable key.
#
# NOTE on the parameter name: it is $Root, NOT $Host. $Host is a READ-ONLY
# automatic variable in PowerShell -- using it as a parameter name makes the
# assignment fail with "无法覆盖变量 Host"，and with $ErrorActionPreference =
# 'Stop' that takes the whole script down.
function Get-ToolboxTrayButtonIn($Root) {
    if (-not $Root) { return $null }
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $script:TrayAE::ControlTypeProperty, $script:TrayCT::Button)
    foreach ($b in $Root.FindAll($script:TrayTS::Descendants, $cond)) {
        if ($b.Current.ClassName -ne 'SystemTray.NormalButton') { continue }
        if ($b.Current.Name -like '*Lingwu*') { return $b }
    }
    return $null
}

# The toolbox's tray icon, wherever the shell is keeping it.
#
# Returns a context object:
#   Button   - the UIA element, or $null
#   Overflow - $true if the overflow flyout had to be opened
#
# THE FLYOUT IS LEFT OPEN when it had to be opened: the element is only
# valid while the flyout is open. The caller MUST call Close-ToolboxTray.
function Get-ToolboxTrayButton {
    $btn = Get-ToolboxTrayButtonIn (Get-TrayTaskbar)
    if ($btn) {
        return [pscustomobject]@{ Button = $btn; Overflow = $false }
    }

    $wasOpen = $null -ne (Get-TrayOverflowWindow)
    if (-not $wasOpen) {
        if (-not (Invoke-TrayChevron)) {
            return [pscustomobject]@{ Button = $null; Overflow = $false }
        }
        Start-Sleep -Milliseconds 800
    }
    $ov = Get-TrayOverflowWindow
    $btn = $null
    if ($ov) { $btn = Get-ToolboxTrayButtonIn $ov }
    return [pscustomobject]@{ Button = $btn; Overflow = (-not $wasOpen) }
}

# Put the desktop back the way Get-ToolboxTrayButton found it.
function Close-ToolboxTray($Ctx) {
    if (-not $Ctx) { return }
    if ($Ctx.Overflow) {
        [void](Invoke-TrayChevron)
        Start-Sleep -Milliseconds 500
    }
}

# Is the toolbox's tray icon there at all? Opens the overflow if needed and
# closes it again.
function Test-ToolboxTrayIconPresent {
    $ctx = Get-ToolboxTrayButton
    try {
        return ($null -ne $ctx.Button)
    } finally {
        Close-ToolboxTray $ctx
    }
}

# Press the toolbox's tray icon (left click semantics) and report whether it
# worked. The flyout is opened if needed and closed again afterwards.
function Invoke-ToolboxTrayButton {
    $ctx = Get-ToolboxTrayButton
    try {
        if (-not $ctx.Button) { return $false }
        return (Invoke-UiaInvoke $ctx.Button)
    } finally {
        Close-ToolboxTray $ctx
    }
}

# The screen rectangle of the toolbox's tray icon, or $null when it cannot
# be read (which is the normal answer for an icon sitting in the overflow
# once the flyout has been closed).
function Get-ToolboxTrayRect {
    $ctx = Get-ToolboxTrayButton
    try {
        if (-not $ctx.Button) { return $null }
        $r = $ctx.Button.GetCurrentPropertyValue(
            $script:TrayAE::BoundingRectangleProperty)
        if ($null -eq $r) { return $null }
        $x = [double]$r.X; $y = [double]$r.Y
        $w = [double]$r.Width; $h = [double]$r.Height
        if ([double]::IsNaN($x) -or [double]::IsInfinity($x) -or
            [double]::IsNaN($y) -or [double]::IsInfinity($y) -or
            $w -le 0 -or $h -le 0) {
            return $null
        }
        return @{ X = [int]$x; Y = [int]$y; W = [int]$w; H = [int]$h }
    } finally {
        Close-ToolboxTray $ctx
    }
}

# One-shot view of the toolbox's tray icon.
#
#   Present - $true when the icon exists (visible area OR overflow flyout)
#   Rect    - the screen rectangle, or $null
#   Overflow- $true when the icon had to be found in the "^" flyout
#   Ctx     - the flyout context, only filled with -KeepOpen
#
# WHY ONE CALL INSTEAD OF Test-... + Get-...:
# both of those open and close the overflow flyout on their own, so calling
# them back to back opens it twice and costs ~2.6 s per poll. This one
# opens it at most once.
#
# ABOUT THE RECTANGLE OF AN OVERFLOW ICON:
# measured 2026-10-24, an icon inside the overflow flyout DOES report a
# usable rectangle (80x80 at 3067,1710 on a 3840x2160 screen) -- as long as
# the flyout is still open. Once the flyout is closed the element dies with
# it and its BoundingRectangle comes back as NaN / Infinity.
# So the rectangle is only good for a click that happens WHILE the flyout is
# open: use -KeepOpen for that, and close it afterwards.
#
# And even with a live rectangle, a synthetic click may not work: Win11's
# tray is an XAML island popup, and SendInput does not drive it on this
# machine (measured: the right-click produced no popup menu at all).
function Get-ToolboxTrayState {
    param([switch]$KeepOpen)

    $ctx = Get-ToolboxTrayButton
    try {
        if (-not $ctx.Button) {
            return [pscustomobject]@{
                Present = $false; Rect = $null; Overflow = $ctx.Overflow; Ctx = $null
            }
        }
        $rect = $null
        $r = $ctx.Button.GetCurrentPropertyValue(
            $script:TrayAE::BoundingRectangleProperty)
        if ($null -ne $r) {
            $x = [double]$r.X; $y = [double]$r.Y
            $w = [double]$r.Width; $h = [double]$r.Height
            if (-not [double]::IsNaN($x) -and -not [double]::IsInfinity($x) -and
                -not [double]::IsNaN($y) -and -not [double]::IsInfinity($y) -and
                $w -gt 0 -and $h -gt 0) {
                $rect = @{ X = [int]$x; Y = [int]$y; W = [int]$w; H = [int]$h }
            }
        }
        if ($KeepOpen) {
            # The flyout stays open. The caller MUST pass $state.Ctx to
            # Close-ToolboxTray when it is done.
            return [pscustomobject]@{
                Present = $true; Rect = $rect; Overflow = $ctx.Overflow; Ctx = $ctx
            }
        }
        return [pscustomobject]@{
            Present = $true; Rect = $rect; Overflow = $ctx.Overflow; Ctx = $null
        }
    } finally {
        if (-not $KeepOpen) { Close-ToolboxTray $ctx }
    }
}
