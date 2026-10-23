# uia.ps1 -- shared helpers for the acceptance scripts. Dot-source it.
#
# Rules that are not negotiable here (each one was learned the hard way):
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

function Stop-App([string]$ProcessName) {
    Get-Process $ProcessName -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
}

function Start-App([string]$Exe) {
    if (-not (Test-Path $Exe)) { throw "Exe not found: $Exe" }
    $p = Start-Process $Exe -PassThru
    Write-Host "launched pid=$($p.Id)"
    return $p
}

# Top-level window of a process, retried until it shows up.
function Get-AppWindow([int]$ProcessId, [int]$TimeoutSec = 45) {
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
    $exclude = @('ServiceList', 'ServiceNameBox')
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
