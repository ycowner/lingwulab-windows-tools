# uia.ps1 -- shared helpers for the acceptance scripts. Dot-source it.
#
# Rules that are not negotiable here (each one was learned the hard way):
#
#  0. ALWAYS run these ELEVATED. The toolbox is requireAdministrator, and
#     UI Automation cannot see across an integrity-level boundary: the same
#     query against the same window returns 451 descendants from an elevated
#     client and 4 from a non-elevated one. Call Require-Elevated (see
#     elevate.ps1, dot-sourced below) right after dot-sourcing this file and
#     it takes care of that.
#
#  1. ALWAYS run these under pwsh, not Windows PowerShell 5.1. 5.1's COM
#     interop binds UIAutomationClient differently and the AndCondition
#     construction below silently degrades to a single condition.
#
#  2. ALWAYS scope element searches to the process id. A PropertyCondition on
#     AutomationId alone matches the *first* window of that name on the
#     desktop, which is very often some other application. AndCondition over
#     (ControlType, ProcessId) is the only form that survives having two
#     instances of anything running.
#
#  3. NEVER trust a null to mean "control absent". Virtualised list items and
#     freshly-activated windows are briefly absent; Get-ById retries for a
#     while and then throws with the id and the elapsed time.
#
#  4. The app is launched and inspected inside ONE script invocation. A GUI
#     process does not outlive a single non-interactive shell call, so
#     "launch in one call, inspect in the next" simply does not work.
#
#  5. No synthetic mouse input anywhere. It cannot be trusted as evidence on
#     this machine; everything below goes through UIA patterns instead
#     (Invoke / Value / SelectionItem / ExpandCollapse), which are the real
#     thing the app responds to anyway.
#
# Pure ASCII on purpose (repo rule for .ps1).

Set-StrictMode -Version Latest

# The non-UI shared bootstrap (project root, exe resolution, elevation)
# lives next to this file so that scripts which do not need UI Automation
# can still use it.
. (Join-Path $PSScriptRoot 'common.ps1')

function Initialize-Uia {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
}

# Load the assemblies BEFORE the type handles below are evaluated. Dot-sourcing
# this file runs top-to-bottom, so deferring this to a caller-called
# Initialize-Uia would make every $script:AE reference throw "type not found"
# the moment the caller touched a helper.
Initialize-Uia

$script:AE = [System.Windows.Automation.AutomationElement]
$script:TS = [System.Windows.Automation.TreeScope]
$script:CT = [System.Windows.Automation.ControlType]
$script:UiaFailures = 0

# NOTE: Stop-App / Start-App live in common.ps1, not here. They are process
# helpers, and scripts that never touch UI Automation still need them.

# Top-level window of a process, retried until it shows up.
#
# The toolbox has more than one top-level window in the same process (main,
# settings, create-service), so "the first Window with this pid" is not
# necessarily the one the caller means. The main window is identified by the
# nav pill it owns: NavServiceButton lives in the shell's title bar, so it
# is present whichever module is showing. Only if that probe fails do we
# fall back to "first window with this pid".
function Get-AppWindow([int]$ProcessId, [int]$TimeoutSec = 45) {
    try {
        return Get-AppWindowByProbe $ProcessId 'NavServiceButton' ([Math]::Min($TimeoutSec, 20))
    } catch {
        # Fall through to the generic search below.
    }
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ControlTypeProperty, $script:CT::Window)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, $ProcessId)))
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $w = $script:AE::RootElement.FindFirst($script:TS::Children, $cond)
        if ($w) {
            # A window can be found before its content is realised. Wait for
            # the title to stop being empty so callers do not race the XAML
            # tree coming up.
            $b = $w.Current.BoundingRectangle
            if ($b.Width -gt 0 -and $b.Height -gt 0) { return $w }
        }
        Start-Sleep -Milliseconds 300
    }
    throw "No window for pid $ProcessId after $TimeoutSec s."
}

# Which module the shell is currently showing.
#
# Both pages are constructed once and kept resident; switching only flips
# Visibility. A collapsed page is NOT in the UI Automation tree at all
# (measured: after switching to the service page, ProcessList and
# AggregateSwitch stop resolving while ServiceList and FilterBox appear),
# so "is ProcessList reachable?" is a reliable way to tell the two apart.
#
# This also settles the AutomationId collision between the two pages:
# SearchBox, HeaderName, StatusLeft and StatusRight exist in both, but only
# one of the two pages is ever in the tree, so a plain AutomationId lookup
# is unambiguous.
function Get-CurrentModule($Win) {
    $probe = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, [int]$Win.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::AutomationIdProperty, 'ProcessList')))
    if ($Win.FindFirst($script:TS::Descendants, $probe)) { return 'Monitor' }
    return 'Service'
}

# Switch the shell to a module and wait for the page to come up.
#
# The nav pill is a Button, so InvokePattern is the right way to press it.
# Clicking the pill of the module that is already showing is harmless --
# ShowModule only flips Visibility, it never rebuilds the page -- but the
# check below keeps the scripts from doing pointless work.
function Switch-Module($Win, [ValidateSet('Monitor', 'Service')][string]$Module) {
    $current = Get-CurrentModule $Win
    if ($current -eq $Module) {
        Write-Host "  already on the $Module page"
        return
    }
    $btnId = if ($Module -eq 'Service') { 'NavServiceButton' } else { 'NavMonitorButton' }
    $btn = Get-ById $Win $btnId
    Invoke-El $btn $btnId
    Start-Sleep -Milliseconds 900
    $now = Get-CurrentModule $Win
    if ($now -ne $Module) {
        throw ("Switched to the $Module page but the tree still shows $now.")
    }
    Write-Host "  on the $Module page"
}

# A specific top-level window of a process, identified by a control it owns.
#
# The app has two windows (main + create-service) in the same process, so
# "the first Window with this pid" is ambiguous. Their titles are Chinese and
# uia.ps1 must stay pure ASCII, so match on a control that only exists in the
# one we want instead. That is also more robust than a title: renaming the
# window would silently break a title-based selector.
function Get-AppWindowByProbe([int]$ProcessId, [string]$ProbeId, [int]$TimeoutSec = 20) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ControlTypeProperty, $script:CT::Window)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, $ProcessId)))
    $probe = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::AutomationIdProperty, $ProbeId)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $wins = $script:AE::RootElement.FindAll($script:TS::Children, $cond)
        for ($i = 0; $i -lt $wins.Count; $i++) {
            if ($wins[$i].FindFirst($script:TS::Descendants, $probe)) {
                return $wins[$i]
            }
        }
        Start-Sleep -Milliseconds 300
    }
    throw "No window of pid $ProcessId contains '$ProbeId' after $TimeoutSec s."
}

# The settings window, but only while it is actually on screen.
#
# The settings window is HIDDEN, not destroyed, when it is closed (see
# SettingsWindow::HideSelf -- destroying it would leak native resources on
# every round). A hidden window can still be reachable through UI
# Automation, so "find the settings window" is not the same question as
# "is the settings window open". Scripts that open it have to be able to
# tell the two apart, hence the visibility check.
#
# The probe is CancelButton, which only the settings window owns.
function Find-VisibleSettingsWindow([int]$ProcessId, [int]$MainHwnd, [int]$TimeoutSec = 15) {
    $winCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ControlTypeProperty, $script:CT::Window)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, $ProcessId)))
    $probe = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::AutomationIdProperty, 'CancelButton')
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $wins = $script:AE::RootElement.FindAll($script:TS::Children, $winCond)
        for ($i = 0; $i -lt $wins.Count; $i++) {
            if ([int]$wins[$i].Current.NativeWindowHandle -eq $MainHwnd) { continue }
            if (-not $wins[$i].FindFirst($script:TS::Descendants, $probe)) { continue }
            $b = $wins[$i].Current.BoundingRectangle
            if ($b.Width -gt 0 -and $b.Height -gt 0 -and -not $wins[$i].Current.IsOffscreen) {
                return $wins[$i]
            }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# A ContentDialog in the same process, found by probing rather than by title.
#
# WinUI's ContentDialog is hosted in its own popup, so it may or may not be a
# direct child of the main window. Walking the whole desktop filtered by
# process id and picking the window that owns a Button is the only shape that
# survives both hosting models. Titles are Chinese and this file is ASCII, so
# the title is not an option.
function Get-Dialog($Root, [int]$TimeoutSec = 15) {
    $pid_ = [int]$Root.Current.ProcessId
    $winCond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ControlTypeProperty, $script:CT::Window)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, $pid_)))
    # The main window AND the create-service window both have several buttons,
    # so "a window with buttons" matches either of them and then reports their
    # toolbar as if it were the dialog's contents. Exclude both by the one
    # control each is uniquely known by.
    #
    # The main window is excluded by NavServiceButton, which lives in the
    # shell's title bar and is therefore present whichever module is
    # showing; the list ids are there as a belt-and-braces second marker.
    $exclude = @('NavServiceButton', 'ServiceList', 'ProcessList', 'ServiceNameBox')
    $selfConds = @()
    foreach ($id in $exclude) {
        $selfConds += New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::AutomationIdProperty, $id)
    }
    $btnCond = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::ControlTypeProperty, $script:CT::Button)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $candidates = @($script:AE::RootElement.FindAll($script:TS::Children, $winCond))
        $candidates += @($Root.FindAll($script:TS::Descendants, $winCond))
        foreach ($w in $candidates) {
            $isAppWindow = $false
            foreach ($c in $selfConds) {
                if ($w.FindFirst($script:TS::Descendants, $c)) { $isAppWindow = $true; break }
            }
            if ($isAppWindow) { continue }
            $btns = $w.FindAll($script:TS::Descendants, $btnCond)
            if ($btns.Count -ge 2) { return $w }
        }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

# Full descendant dump of an element. Used when a dialog's structure has to be
# read rather than guessed at -- the labels are localised, so the only honest
# way to check them is to look.
function Dump-Tree($El, [string]$Indent = '    ') {
    $all = $El.FindAll($script:TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    Write-Host "descendants = $($all.Count)"
    for ($i = 0; $i -lt $all.Count; $i++) {
        $e = $all[$i]
        Write-Host ("{0}[{1,2}] {2,-16} '{3}'" -f $Indent, $i,
            ($e.Current.ControlType.ProgrammaticName -replace 'ControlType.', ''),
            $e.Current.Name)
    }
}

# Button names of a dialog, read only once the tree has stopped changing.
#
# A WinUI ContentDialog populates asynchronously: the instant the window has
# two buttons it is not necessarily the two you get a second later. Sampling
# too early yields plausible-but-wrong labels (the content TextBlock read as a
# button, and WinUI's default "Close" instead of the real CloseButtonText),
# which then gets asserted on. So compare two consecutive samples and return
# only once they agree.
function Get-DialogButtons($Dlg, [int]$StableSec = 3, [int]$TimeoutSec = 15) {
    $btnCond = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::ControlTypeProperty, $script:CT::Button)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $previous = $null
    $sameSince = $null
    while ((Get-Date) -lt $deadline) {
        $items = $Dlg.FindAll($script:TS::Descendants, $btnCond)
        $names = @()
        for ($i = 0; $i -lt $items.Count; $i++) { $names += $items[$i].Current.Name }
        $key = ($names -join "`u{001F}")
        if ($null -ne $previous -and $key -eq $previous) {
            if ($null -eq $sameSince) { $sameSince = Get-Date }
            if (((Get-Date) - $sameSince).TotalSeconds -ge $StableSec) { return , $names }
        } else {
            $previous = $key
            $sameSince = $null
        }
        Start-Sleep -Milliseconds 400
    }
    $items = $Dlg.FindAll($script:TS::Descendants, $btnCond)
    $names = @()
    for ($i = 0; $i -lt $items.Count; $i++) { $names += $items[$i].Current.Name }
    return , $names
}

function Show-Codepoints([string]$Text) {
    return (($Text.ToCharArray() | ForEach-Object { [int]$_ }) -join ',')
}

# Search inside a process window only, and only for one AutomationId.
function Get-ById($Root, [string]$Id, [int]$TimeoutSec = 10) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, [int]$Root.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::AutomationIdProperty, $Id)))
    $scope = if ($script:AE::ControlTypeProperty -and $Root.Current.ControlType -eq $script:CT::Window) {
        $script:TS::Descendants
    } else { $script:TS::Descendants }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $tries = 0
    while ((Get-Date) -lt $deadline) {
        $tries++
        $e = $Root.FindFirst($scope, $cond)
        if ($e) { return $e }
        Start-Sleep -Milliseconds 250
    }
    throw "AutomationId '$Id' not found after $TimeoutSec s ($tries tries) in pid $($Root.Current.ProcessId)."
}

function Get-ByName($Root, [string]$Name, [int]$TimeoutSec = 10) {
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ProcessIdProperty, [int]$Root.Current.ProcessId)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::NameProperty, $Name)))
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $e = $Root.FindFirst($script:TS::Descendants, $cond)
        if ($e) { return $e }
        Start-Sleep -Milliseconds 250
    }
    throw "Name '$Name' not found after $TimeoutSec s in pid $($Root.Current.ProcessId)."
}

function Invoke-El($El, [string]$What) {
    # A disabled control's InvokePattern throws a bare COM "unrecognised error"
    # that says nothing about the real problem. Say it in words instead.
    if (-not $El.Current.IsEnabled) {
        throw "Cannot invoke '$What': the control is disabled."
    }
    $pat = $El.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pat.Invoke()
    Write-Host "  invoked: $What"
}

# A service name that is actually Running, so operations that only make sense
# on a running service (Stop, Restart) can be exercised. Asking sc.exe beats
# guessing from a row's synthesised UIA name, whose text is localised.
function Get-RunningServiceName([int]$Limit = 20) {
    $names = @()
    foreach ($s in (Get-Service -ErrorAction SilentlyContinue |
                    Where-Object { $_.Status -eq 'Running' } |
                    Sort-Object -Property Name)) {
        $names += $s.Name
        if ($names.Count -ge $Limit) { break }
    }
    return , $names
}

function Set-ElText($El, [string]$Text, [string]$What) {
    $pat = $El.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $pat.SetValue($Text)
    Write-Host "  set '$What' = '$Text'"
}

function Get-ElText($El) {
    try {
        $pat = $El.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        return $pat.Current.Value
    } catch {
        return $El.Current.Name
    }
}

function Get-Text($Root, [string]$Id, [int]$TimeoutSec = 10) {
    return (Get-ElText (Get-ById $Root $Id $TimeoutSec))
}

# Expand an Expander so its content becomes reachable through UIA.
#
# A collapsed Expander does not expose its children at all -- FindFirst returns
# nothing and any script that goes looking for a field inside one fails with
# "AutomationId not found" even though the object exists and the app itself
# can call into it. So expand before asking for anything inside.
function Expand-Panel($Root, [string]$Id = 'DetailExpander') {
    $exp = Get-ById $Root $Id
    $pat = $exp.GetCurrentPattern(
        [System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    if ($pat.Current.ExpandCollapseState -ne
        [System.Windows.Automation.ExpandCollapseState]::Expanded) {
        $pat.Expand()
        Write-Host "  expanded '$Id'"
    }
    # Content is created on expand; give it a moment to enter the tree.
    Start-Sleep -Milliseconds 600
    return $exp
}

function Select-ComboIndexLegacyRemoved() { }

# Names of the ListView rows, top to bottom.
#
# Limit comes BEFORE ListId on purpose. With the list id first, a natural
# call like `Get-RowNames $win 6` binds 6 to the ListId string and the helper
# then hunts for a ListView whose AutomationId is literally "6" -- which fails
# with a baffling "AutomationId '6' not found" instead of "you passed the
# arguments in the wrong order".
function Get-RowNames($Root, [int]$Limit = 12, [string]$ListId = 'ServiceList') {
    $list = Get-ById $Root $ListId
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::ControlTypeProperty, $script:CT::ListItem)
    $items = $list.FindAll($script:TS::Descendants, $cond)
    $out = @()
    for ($i = 0; $i -lt [Math]::Min($Limit, $items.Count); $i++) {
        $out += $items[$i].Current.Name
    }
    return $out
}

function Select-RowByName($Root, [string]$RowName, [string]$ListId = 'ServiceList') {
    $list = Get-ById $Root $ListId
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::ControlTypeProperty, $script:CT::ListItem)),
        (New-Object System.Windows.Automation.PropertyCondition(
            $script:AE::NameProperty, $RowName)))
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        $e = $list.FindFirst($script:TS::Descendants, $cond)
        if ($e) {
            $sel = $e.GetCurrentPattern(
                [System.Windows.Automation.SelectionItemPattern]::Pattern)
            $sel.Select()
            Write-Host "  selected row '$RowName'"
            return $e
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Row '$RowName' not found in $ListId."
}

function Show-Rows([string[]]$Rows, [string]$Indent = '    ') {
    $n = 0
    foreach ($r in $Rows) { Write-Host "$Indent[$n] $r"; $n++ }
}

# Realised row count. Read it, but never assert on it.
#
# A WinUI ListView is virtualised: only the containers the user has scrolled
# past exist as UI Automation children. On the reference machine the list
# holds 345 services but only ~25 rows are realised, and the number *grows*
# as the user scrolls -- so a "before" and an "after" comparison of this
# value measures scrolling, not filtering. Anything that means "how many
# services are in the list" has to come from the status bar instead.
function Get-RowCount($Root, [string]$ListId = 'ServiceList') {
    $list = Get-ById $Root $ListId
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::ControlTypeProperty, $script:CT::ListItem)
    return $list.FindAll($script:TS::Descendants, $cond).Count
}

# Counts out of the status bar, e.g. "345 services / 345 shown".
#
# The label is Chinese, so this matches on digits rather than on the text
# itself; that keeps uia.ps1 pure ASCII (repo rule) and immune to a wording
# change in the XAML.
function Get-StatusCounts($Root) {
    $raw = Get-Text $Root 'StatusLeft'
    $nums = @([regex]::Matches($raw, '\d+') | ForEach-Object { [int]$_.Value })
    # Both properties exist from the start. Creating them conditionally with
    # Add-Member and reading them back looks harmless, but under
    # Set-StrictMode -Version Latest (which this file sets) the very first
    # read of a property that does not exist yet throws.
    return [pscustomobject]@{
        Raw     = $raw
        Numbers = $nums
        Total   = $(if ($nums.Count -ge 1) { $nums[0] } else { 0 })
        Shown   = $(if ($nums.Count -ge 2) { $nums[1] } else { 0 })
    }
}

# ComboBox items only exist as UIA children while the popup is open, so
# counting them on a closed combo always yields 0.
function Get-ComboItems($Root, [string]$Id) {
    $combo = Get-ById $Root $Id
    $itemCond = New-Object System.Windows.Automation.PropertyCondition(
        $script:AE::ControlTypeProperty, $script:CT::ListItem)

    $before = $combo.FindAll($script:TS::Descendants, $itemCond).Count
    if ($before -gt 0) { return , @($combo.FindAll($script:TS::Descendants, $itemCond)) }

    $opened = $false
    try {
        $ec = $combo.GetCurrentPattern(
            [System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        $ec.Expand()
        $opened = $true
    } catch {
        throw "ComboBox '$Id' exposes neither items nor an ExpandCollapsePattern."
    }

    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        $items = $combo.FindAll($script:TS::Descendants, $itemCond)
        if ($items.Count -gt 0) {
            $out = @()
            for ($i = 0; $i -lt $items.Count; $i++) { $out += $items[$i] }
            try { $ec.Collapse() } catch { }
            return , $out
        }
        Start-Sleep -Milliseconds 200
    }
    try { if ($opened) { $ec.Collapse() } } catch { }
    throw "ComboBox '$Id' stayed empty after Expand()."
}

function Select-ComboIndex($Root, [string]$Id, [int]$Index) {
    $items = Get-ComboItems $Root $Id
    if ($Index -ge $items.Count) {
        throw "ComboBox '$Id' has $($items.Count) items; index $Index requested."
    }
    $sel = $items[$Index].GetCurrentPattern(
        [System.Windows.Automation.SelectionItemPattern]::Pattern)
    $sel.Select()
    $label = $items[$Index].Current.Name
    Write-Host "  selected '$Id' index $Index ($label)"
}

function Assert-True([bool]$Ok, [string]$What) {
    if ($Ok) {
        Write-Host "  [ok]   $What"
    } else {
        Write-Host "  [FAIL] $What"
        $script:UiaFailures++
    }
}

function Write-Summary([string]$Name) {
    if ($script:UiaFailures -eq 0) {
        Write-Host "`nPASS: $Name"
        exit 0
    }
    Write-Host "`nFAIL: $Name ($script:UiaFailures failure(s))"
    exit 1
}
