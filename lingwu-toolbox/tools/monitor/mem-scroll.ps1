param(
    [string]$Exe,
    [int]$Seconds = 120,
    [int]$ScrollAt = 10,
    [string]$OutCsv = "$env:TEMP\mem_scroll.csv"
)

# 判定「t=56 那次 +26 MB 一次性阶跃是不是滚动/布局触发」。
# 做法：启动后 $ScrollAt 秒，用 UIA ScrollPattern 把列表滚到底再滚回顶部，
# 之后继续采样到 $Seconds。若阶跃时点跟着提前到 $ScrollAt 附近，即证实。
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
$samples = New-Object System.Collections.Generic.List[object]
$scrolled = $false

function Get-Win($pid_) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    for ($i = 0; $i -lt 60; $i++) {
        $w = $AE::RootElement.FindFirst($TS::Children, $cond)
        if ($w) { return $w }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

for ($i = 1; $i -le $Seconds; $i++) {
    Start-Sleep -Seconds 1

    if (-not $scrolled -and $i -ge $ScrollAt) {
        $scrolled = $true
        $win = Get-Win $p.Id
        if ($win) {
            $lc = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, 'ProcessList')
            $list = $win.FindFirst($TS::Descendants, $lc)
            if ($list) {
                $sp = $null
                try { $sp = $list.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern) } catch {}
                if ($sp) {
                    Write-Output ("t={0}s SCROLL to bottom: h={1:N0}% v={2:N0}%" -f $i,
                        ($sp.Current.HorizontalScrollPercent), ($sp.Current.VerticalScrollPercent))
                    if ($sp.Current.VerticallyScrollable) { $sp.ScrollVertical([System.Windows.Automation.ScrollAmount]::LargeIncrement) }
                    Start-Sleep -Milliseconds 600
                    if ($sp.Current.VerticallyScrollable) { $sp.ScrollVertical([System.Windows.Automation.ScrollAmount]::LargeIncrement) }
                    Start-Sleep -Milliseconds 600
                    Write-Output ("t={0}s SCROLL back to top" -f $i)
                    # ScrollAmount 枚举没有 Top/Bottom，只有
                    # LargeDecrement/SmallDecrement/NoAmount/LargeIncrement/SmallIncrement，
                    # 所以「回顶」只能用 LargeDecrement 反复往上翻。
                    for ($k = 0; $k -lt 60; $k++) {
                        if (-not $sp.Current.VerticallyScrollable) { break }
                        if ($sp.Current.VerticalScrollPercent -le 0.0) { break }
                        $sp.ScrollVertical([System.Windows.Automation.ScrollAmount]::LargeDecrement)
                    }
                } else {
                    Write-Output ("t={0}s NO ScrollPattern on ProcessList" -f $i)
                }
            } else {
                Write-Output ("t={0}s ProcessList NOT FOUND" -f $i)
            }
        }
    }

    $live = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $live) {
        $samples.Add([pscustomobject]@{ t = $i; priv = -1; ws = -1; h = -1; note = 'GONE' })
        break
    }
    $samples.Add([pscustomobject]@{
        t    = $i
        priv = [math]::Round($live.PrivateMemorySize64 / 1MB, 1)
        ws   = [math]::Round($live.WorkingSet64 / 1MB, 1)
        h    = $live.HandleCount
        note = ''
    })
}

$samples | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8
$alive = $samples | Where-Object { $_.priv -ge 0 }
if (-not $alive) { Write-Output 'PROCESS NEVER SAMPLED'; exit 1 }

Write-Output ("samples={0} span={1}s scrolledAt={2}s" -f $alive.Count, $alive[-1].t, $ScrollAt)
$prev = $null
foreach ($s in $alive) {
    if ($null -eq $prev) { Write-Output ("  t={0,3}s priv={1,6} ws={2,6} h={3}" -f $s.t, $s.priv, $s.ws, $s.h) }
    elseif ([math]::Abs($s.priv - $prev.priv) -ge 1.0) {
        Write-Output ("  t={0,3}s priv={1,6} ws={2,6} h={3}  <-- step {4:+#.#;-#.#}MB" -f $s.t, $s.priv, $s.ws, $s.h, ($s.priv - $prev.priv))
    }
    $prev = $s
}
Write-Output ("last: t={0}s priv={1}MB ws={2}MB h={3}" -f $alive[-1].t, $alive[-1].priv, $alive[-1].ws, $alive[-1].h)
Write-Output "CSV=$OutCsv"
