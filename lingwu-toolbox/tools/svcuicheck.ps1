# uicheck.ps1 -- baseline acceptance: window opens, list enumerates, columns sort,
# search filters, status filter switches.
#
#   pwsh -NoProfile -File .\tools\uicheck.ps1
#   pwsh -NoProfile -File .\tools\uicheck.ps1 -Exe .\dist\Release\LingwuServiceManager.exe
#
# This is the smoke test. If it fails, nothing below it is worth running.
# It is also the only script that does not touch service state -- every other
# one either starts/stops a service or creates one.

[CmdletBinding()]
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Release\LingwuServiceManager.exe')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'uia.ps1')

$script:UiaFailures = 0

Write-Host '=== uicheck: baseline ==='
Stop-App 'LingwuServiceManager'
$p = Start-App $Exe

# try/finally so a thrown assertion still tears the process down. Without it a
# failed run leaves LingwuServiceManager.exe running, and the *next* build fails
# with LNK1104 "cannot open file" -- an error that points at the linker while
# the real cause is the previous test run.
try {
    $win = Get-AppWindow $p.Id 60

Write-Host "`n--- window ---"
$r = $win.Current.BoundingRectangle
Write-Host ("  rect  : {0}x{1} at ({2},{3})" -f [int]$r.Width, [int]$r.Height, [int]$r.X, [int]$r.Y)
Write-Host ("  title bytes: " + (($win.Current.Name.ToCharArray() | ForEach-Object { [int]$_ }) -join ','))
Assert-True ($r.Width -gt 600 -and $r.Height -gt 400) 'window has a usable size'

# Wait for the poll thread's first full pass. The status bar is the source of
# truth for "how many services are in the list" -- see Get-StatusCounts.
$st = $null
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
    $st = Get-StatusCounts $win
    if ($st.Total -gt 0) { break }
    Start-Sleep -Milliseconds 500
}

Write-Host "`n--- list ---"
Write-Host "  status  : $($st.Raw)"
Write-Host "  total   : $($st.Total)"
Write-Host "  shown   : $($st.Shown)"
Write-Host "  realised: $(Get-RowCount $win)  (virtualised; informational only)"
Assert-True ($st.Total -gt 100) 'status bar reports more than 100 services (enumeration ran)'
Assert-True ($st.Shown -eq $st.Total) 'default view shows every service (no filter)'
Show-Rows (Get-RowNames $win) '    '

$baseline = $st.Total

# Ordering checks work on the *names* of the realised rows. The list is
# virtualised, so only the top slice is ever realised -- but that slice is
# exactly what the ordering question is about.
function Test-Ordered([string[]]$Names, [bool]$Descending) {
    for ($i = 1; $i -lt $Names.Count; $i++) {
        $cmp = [string]::Compare($Names[$i - 1], $Names[$i],
                                 [StringComparison]::OrdinalIgnoreCase)
        if ($Descending) { if ($cmp -lt 0) { return $false } }
        else { if ($cmp -gt 0) { return $false } }
    }
    return $true
}

Write-Host "`n--- TEST 1: name header toggles to descending ---"
Invoke-El (Get-ById $win 'HeaderName') 'HeaderName'
Start-Sleep -Seconds 2
$names = Get-RowNames $win
Show-Rows $names '    '
Assert-True (Test-Ordered $names $true) 'name header sorts descending on the first click'

Write-Host "`n--- TEST 2: name header toggles back to ascending ---"
Invoke-El (Get-ById $win 'HeaderName') 'HeaderName'
Start-Sleep -Seconds 2
$names = Get-RowNames $win
Show-Rows $names '    '
Assert-True (Test-Ordered $names $false) 'clicking it again sorts ascending'

Write-Host "`n--- TEST 3: state header ---"
$before = (Get-RowNames $win) -join '|'
Invoke-El (Get-ById $win 'HeaderState') 'HeaderState'
Start-Sleep -Seconds 2
$after = (Get-RowNames $win) -join '|'
Show-Rows (Get-RowNames $win) '    '
Assert-True ($before -ne $after) 'the state header reorders the visible rows'

Write-Host "`n--- TEST 4: start-type header ---"
$before = (Get-RowNames $win) -join '|'
Invoke-El (Get-ById $win 'HeaderStartType') 'HeaderStartType'
Start-Sleep -Seconds 2
$after = (Get-RowNames $win) -join '|'
Show-Rows (Get-RowNames $win) '    '
Assert-True ($before -ne $after) 'the start-type header reorders the visible rows'

Write-Host "`n--- back to name ascending ---"
Invoke-El (Get-ById $win 'HeaderName') 'HeaderName'
Start-Sleep -Seconds 2
$names = Get-RowNames $win
Assert-True (Test-Ordered $names $false) 'switching back to the name column restores ascending order'

Write-Host "`n--- TEST 3: search box ---"
$box = Get-ById $win 'SearchBox'
$editCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Edit)
$inner = $box.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $editCond)
if ($null -eq $inner) { $inner = $box }
Set-ElText $inner 'RpcSs' 'SearchBox'
Start-Sleep -Seconds 3
$st = Get-StatusCounts $win
Write-Host "  status : $($st.Raw)"
Write-Host "  shown  : $($st.Shown)"
Assert-True ($st.Shown -lt $baseline) 'search narrows the shown count'
Assert-True ($st.Shown -gt 0) 'search still matches at least one service'

Write-Host "`n--- TEST 4: clear search ---"
Set-ElText $inner '' 'SearchBox'
Start-Sleep -Seconds 3
$st = Get-StatusCounts $win
Write-Host "  shown  : $($st.Shown)"
Assert-True ($st.Shown -eq $baseline) 'clearing the search restores every row'

Write-Host "`n--- TEST 5: status filter combo ---"
$items = Get-ComboItems $win 'FilterBox'
Write-Host "  options = $($items.Count)"
for ($i = 0; $i -lt $items.Count; $i++) { Write-Host "    [$i] $($items[$i].Current.Name)" }
Assert-True ($items.Count -ge 4) 'FilterBox exposes its options'

Select-ComboIndex $win 'FilterBox' 1
Start-Sleep -Seconds 3
$st = Get-StatusCounts $win
Write-Host "  shown after filter[1] : $($st.Shown)"
Assert-True ($st.Shown -lt $baseline) 'a narrower filter reduces the shown count'

Select-ComboIndex $win 'FilterBox' 0
Start-Sleep -Seconds 3
$st = Get-StatusCounts $win
Assert-True ($st.Shown -eq $baseline) 'switching back to "all" restores every row'

Write-Host "`n--- TEST 6: detail panel ---"
Expand-Panel $win 'DetailExpander' | Out-Null
$names = Get-RowNames $win
Select-RowByName $win $names[0] | Out-Null
Start-Sleep -Seconds 3
$bp = Get-Text $win 'DetailBinaryPath'
$acct = Get-Text $win 'DetailAccount'
Write-Host "  binaryPath : $bp"
Write-Host "  account    : $acct"
Assert-True (-not [string]::IsNullOrWhiteSpace($bp)) 'detail panel fills in the binary path'
Assert-True (-not [string]::IsNullOrWhiteSpace($acct)) 'detail panel fills in the account'

Write-Host "`n--- memory ---"
$p.Refresh()
Write-Host ("  priv={0} MB  ws={1} MB  handles={2}" -f `
    [Math]::Round($p.PrivateMemorySize64 / 1MB, 1),
    [Math]::Round($p.WorkingSet64 / 1MB, 1), $p.HandleCount)
} finally {
    Stop-App 'LingwuServiceManager'
}

Write-Summary 'uicheck'
