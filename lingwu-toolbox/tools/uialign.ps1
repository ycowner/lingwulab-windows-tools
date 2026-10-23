param(
    [string]$Exe,
    [int]$TopRows = 10,        # how many top rows to assert on
    [int]$BottomRows = 3,      # how many rows to assert after scrolling to the end
    [double]$TolPx = 1.0,      # tolerance in PHYSICAL px (screen pixels, not DIP)
    [int]$SettleSeconds = 14,  # let the refresh loop reach steady state
    [switch]$KillAtEnd       # close the app when finished (default: leave it up)
)

# Table column alignment acceptance test.
#
# Purpose: prove that the right edge of every header label equals the right
# edge of the matching data value, on a real running build, measured through
# UI Automation. Before the fix the three right-hand columns (CPU / Memory /
# Process count) sat tens of DIPs to the LEFT of their data values.
#
# Design notes / constraints learned the hard way:
#
#   * Assertions compare two measured EDGES against each other. They never
#     compare against a hard-coded pixel value, so the test stays valid at any
#     DPI scale and any window width.
#
#   * Cross-process element lookup MUST combine ControlType and ProcessId.
#     Matching on ControlType alone from the desktop root also matches other
#     running applications, and clicking one of those has real side effects.
#
#   * Non-interactive shells: a GUI process does not survive between separate
#     tool calls, so launch / measure / assert all live in this one script.
#
#   * The file is pure ASCII on purpose (repo rule). Header labels are located
#     by AutomationId, never by their localized display text.
#
#   * Virtualized (recycled / not yet realized) list items report an infinite
#     BoundingRectangle; a plain [int] cast on that throws and kills the dump.
#     Clamp to a valid range and skip those entries.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]

$script:Failures = New-Object System.Collections.Generic.List[string]
$script:MaxDelta = 0.0

function Clamp-Int([double]$v) {
    if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { return -1 }
    if ($v -lt [int]::MinValue) { return [int]::MinValue }
    if ($v -gt [int]::MaxValue) { return [int]::MaxValue }
    return [int]$v
}

function Check-Delta([string]$what, [double]$a, [double]$b) {
    $d = [math]::Abs($a - $b)
    if ($d -gt $script:MaxDelta) { $script:MaxDelta = $d }
    $ok = $d -le $TolPx
    $mark = 'PASS'
    if (-not $ok) { $mark = 'FAIL'; $script:Failures.Add("$what delta=$([math]::Round($d,2))px") }
    Write-Host ("  {0}  {1,-34} A={2,8:N1} B={3,8:N1} d={4,6:N2}px" -f $mark, $what, $a, $b, $d)
    return $ok
}

# ---------------------------------------------------------------- launch ----

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
Write-Host "launched pid=$($p.Id)"

$win = $null
$cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)
for ($i = 0; $i -lt 60; $i++) {
    $win = $AE::RootElement.FindFirst($TS::Children, $cond)
    if ($win) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $win) { Write-Host 'WINDOW NOT FOUND'; exit 1 }
Write-Host ("window rect = {0}" -f $win.Current.BoundingRectangle)

Start-Sleep -Seconds $SettleSeconds

function Find-Id($root, [string]$id) {
    $c = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $id)
    return $root.FindFirst($TS::Descendants, $c)
}

# ------------------------------------------------------------- measurement ---

# Right edge of the label text inside a header button.
# The button itself is stretched over the whole column, so its own right edge
# is useless for this test; we need the TEXT right edge.
function Get-HeaderTextEdges($win, [hashtable]$ids) {
    $res = @{}
    foreach ($k in $ids.Keys) {
        $btn = Find-Id $win $ids[$k]
        if (-not $btn) { return $null }
        $tc = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text)
        $txt = $btn.FindFirst($TS::Descendants, $tc)
        if (-not $txt) { return $null }
        $br = $txt.Current.BoundingRectangle
        if ([double]::IsInfinity($br.Right)) { return $null }
        $res[$k] = @{
            text = $txt.Current.Name
            right = $br.Right
            left = $br.Left
            btnLeft = $btn.Current.BoundingRectangle.Left
            btnWidth = $btn.Current.BoundingRectangle.Width
        }
    }
    return $res
}

$hdrIds = @{ cpu = 'HeaderCpu'; mem = 'HeaderMem'; proc = 'HeaderProc'; name = 'HeaderName' }

# Read every realized Text element inside the list, group them into rows by
# their vertical center, then split each row into name + three right columns.
# A row needs all four texts present to be measurable; partially realized rows
# at the viewport edge are skipped rather than guessed at.
function Get-Rows($win) {
    $list = Find-Id $win 'ProcessList'
    if (-not $list) { return @() }
    $tc = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text)
    $texts = $list.FindAll($TS::Descendants, $tc)

    $buckets = @{}
    foreach ($t in $texts) {
        $r = $t.Current.BoundingRectangle
        if ([double]::IsInfinity($r.X) -or [double]::IsInfinity($r.Y)) { continue }
        if ($r.Width -le 0 -or $r.Height -le 0) { continue }
        $midY = Clamp-Int ($r.Y + $r.Height / 2.0)
        # Bucket by the vertical center first; adjacent buckets whose averages
        # land within 4 px are merged further down, which keeps a row intact
        # when its four texts wobble by a pixel each.
        if (-not $buckets.ContainsKey($midY)) { $buckets[$midY] = New-Object System.Collections.Generic.List[object] }
        $buckets[$midY].Add([pscustomobject]@{
            text  = $t.Current.Name
            left  = $r.Left
            right = $r.Right
            midY  = ($r.Y + $r.Height / 2.0)
        })
    }

    $rows = New-Object System.Collections.Generic.List[object]
    $keys = $buckets.Keys | Sort-Object
    # Merge buckets whose centers are within 4 px of each other (partial rows
    # near the viewport edge can land on either side of a bucket boundary).
    $merged = New-Object System.Collections.Generic.List[object]
    foreach ($k in $keys) {
        $center = $k
        if ($buckets[$k].Count -gt 0) { $center = ($buckets[$k] | Measure-Object -Property midY -Average).Average }
        if ($merged.Count -gt 0 -and [math]::Abs($center - $merged[$merged.Count - 1].center) -le 4.0) {
            foreach ($it in $buckets[$k]) { $merged[$merged.Count - 1].items.Add($it) }
        } else {
            $merged.Add([pscustomobject]@{
                center = $center
                items  = (New-Object System.Collections.Generic.List[object])
            })
            foreach ($it in $buckets[$k]) { $merged[$merged.Count - 1].items.Add($it) }
        }
    }

    foreach ($g in $merged) {
        if ($g.items.Count -lt 4) { continue }
        $sorted = $g.items | Sort-Object left
        # Column 0 = name (widest, leftmost), then the three right-aligned
        # columns in visual order: cpu, memory, process count.
        $rows.Add([pscustomobject]@{
            nameLeft = $sorted[0].left
            nameText = $sorted[0].text
            cpu      = $sorted[$sorted.Count - 3].right
            mem      = $sorted[$sorted.Count - 2].right
            proc     = $sorted[$sorted.Count - 1].right
            count    = $sorted.Count
        })
    }
    return $rows
}

function Show-Rows($rows, [int]$limit) {
    $n = [Math]::Min($limit, $rows.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $r = $rows[$i]
        Write-Host ("    row{0,-3} nameLeft={1,7:N0} cpu={2,7:N0} mem={3,7:N0} proc={4,7:N0}  ({5})" -f `
            $i, $r.nameLeft, $r.cpu, $r.mem, $r.proc, $r.nameText)
    }
}

function Assert-Block($win, [object[]]$rows, [int]$limit, [string]$tag) {
    Write-Host "--- $tag : asserting $limit row(s) ---"
    if ($rows.Count -eq 0) { Write-Host "  FAIL  no rows realized"; $script:Failures.Add("$tag : no rows"); return }

    $hdr = Get-HeaderTextEdges $win $hdrIds
    if (-not $hdr) { Write-Host '  FAIL  header labels not found'; $script:Failures.Add("$tag : header missing"); return }

    Write-Host ("  header CPU    '{0}' right={1:N1} (btn left={2:N1} w={3:N1})" -f $hdr.cpu.text, $hdr.cpu.right, $hdr.cpu.btnLeft, $hdr.cpu.btnWidth)
    Write-Host ("  header MEM    '{0}' right={1:N1} (btn left={2:N1} w={3:N1})" -f $hdr.mem.text, $hdr.mem.right, $hdr.mem.btnLeft, $hdr.mem.btnWidth)
    Write-Host ("  header PROC   '{0}' right={1:N1} (btn left={2:N1} w={3:N1})" -f $hdr.proc.text, $hdr.proc.right, $hdr.proc.btnLeft, $hdr.proc.btnWidth)
    Write-Host ("  header NAME   '{0}' left ={1:N1}" -f $hdr.name.text, $hdr.name.left)

    $n = [Math]::Min($limit, $rows.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $r = $rows[$i]
        [void](Check-Delta ("$tag row$i cpu hdr-vs-val") $hdr.cpu.right $r.cpu)
        [void](Check-Delta ("$tag row$i mem hdr-vs-val") $hdr.mem.right $r.mem)
        [void](Check-Delta ("$tag row$i proc hdr-vs-val") $hdr.proc.right $r.proc)
        [void](Check-Delta ("$tag row$i name hdr-vs-val") $hdr.name.left $r.nameLeft)
    }

    # Row-to-row consistency: every row's value right edge must be identical.
    if ($n -ge 2) {
        foreach ($col in @('cpu', 'mem', 'proc')) {
            $vals = @()
            for ($i = 0; $i -lt $n; $i++) { $vals += [double]$rows[$i].$col }
            $spread = ($vals | Measure-Object -Maximum).Maximum - ($vals | Measure-Object -Minimum).Minimum
            $ok = $spread -le $TolPx
            $mark = 'PASS'; if (-not $ok) { $mark = 'FAIL'; $script:Failures.Add("$tag $col row spread=$([math]::Round($spread,2))px") }
            if ($spread -gt $script:MaxDelta) { $script:MaxDelta = $spread }
            Write-Host ("  {0}  {1,-34} spread={2,6:N2}px" -f $mark, "$tag $col across $n rows", $spread)
        }
    }
}

function Scroll-ToBottom($win) {
    $list = Find-Id $win 'ProcessList'
    if (-not $list) { return 'ProcessList not found' }
    $sp = $null
    try { $sp = $list.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern) } catch {}
    if (-not $sp) { return 'no ScrollPattern' }
    # ScrollAmount has no Top/Bottom, only increments/decrements, so "to the
    # bottom" means repeated LargeIncrement until the percent stops moving.
    for ($k = 0; $k -lt 200; $k++) {
        if (-not $sp.Current.VerticallyScrollable) { break }
        $before = $sp.Current.VerticalScrollPercent
        $sp.ScrollVertical([System.Windows.Automation.ScrollAmount]::LargeIncrement)
        Start-Sleep -Milliseconds 120
        if ($sp.Current.VerticalScrollPercent -eq $before) { break }
    }
    Start-Sleep -Milliseconds 800
    return ("vScrollPercent={0:N1}" -f $sp.Current.VerticalScrollPercent)
}

# --------------------------------------------------------------- run tests ---

Write-Host ''
Write-Host '=== TEST 1: top rows, default aggregate mode ==='
$rows = Get-Rows $win
Write-Host "  realized rows with 4 texts = $($rows.Count)"
Show-Rows $rows 5
Assert-Block $win $rows $TopRows 'top'

Write-Host ''
Write-Host '=== TEST 2: after scrolling to the bottom ==='
Write-Host ("  " + (Scroll-ToBottom $win))
$rowsBot = Get-Rows $win
Write-Host "  realized rows after scroll = $($rowsBot.Count)"
Show-Rows $rowsBot $BottomRows
Assert-Block $win $rowsBot $BottomRows 'bottom'

Write-Host ''
Write-Host '=== TEST 3: switch to per-process mode ==='
$agg = Find-Id $win 'AggregateSwitch'
if (-not $agg) {
    Write-Host '  FAIL  AggregateSwitch not found'
    $script:Failures.Add('AggregateSwitch missing')
} else {
    $tg = $agg.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    Write-Host ("  AggregateSwitch.ToggleState = " + $tg.Current.ToggleState)
    $tg.Toggle()
    Start-Sleep -Seconds 4
    Write-Host ("  AggregateSwitch.ToggleState = " + $tg.Current.ToggleState)
    $rows2 = Get-Rows $win
    Write-Host "  realized rows in per-process mode = $($rows2.Count)"
    Show-Rows $rows2 3
    Assert-Block $win $rows2 5 'perproc'
}

Write-Host ''
Write-Host '=== PROCESS ==='
$p.Refresh()
Write-Host ("  priv={0}MB ws={1}MB handles={2}" -f `
    [math]::Round($p.PrivateMemorySize64 / 1MB, 1), `
    [math]::Round($p.WorkingSet64 / 1MB, 1), $p.HandleCount)

Write-Host ''
Write-Host ("MAX DELTA over all assertions = {0:N2}px (tolerance {1:N2}px)" -f $script:MaxDelta, $TolPx)

$code = 0
if ($script:Failures.Count -eq 0) {
    Write-Host 'RESULT: PASS - every header label edge matches its data value edge.'
} else {
    $code = 1
    Write-Host ("RESULT: FAIL - {0} assertion(s) violated:" -f $script:Failures.Count)
    foreach ($f in $script:Failures) { Write-Host "  - $f" }
}

if ($KillAtEnd) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Write-Host 'app closed (-KillAtEnd)'
}
exit $code