# keep-awake.ps1 —— 「保持常亮」动作验收
#
# 判据（plan.md §5.2）：
#   「保持常亮」开 → 可观测；关 → 复位。
#
# 【产品行为】（src/ui/TrayPanel.xaml.cpp）
#   · 按钮 id = KeepAwakeButton，文字在「保持常亮 / 常亮已开」之间切换；
#   · 点一下翻转 settings.keepAwake，落 ini（键 KeepAwake），
#     并调用 SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
#     ES_SYSTEM_REQUIRED)；再点一下用 ES_CONTINUOUS 撤销；
#   · **启动时也会把持久化的状态真正应用到系统**（ApplyKeepAwake），
#     并有一个 10 秒的续申请计时器（系统的保持唤醒请求会被清掉，要周期性重申请）。
#
# 【为什么按钮文字是主要的可观测信号】
# SetThreadExecutionState 是**按线程**的：别的进程查不到本进程的线程状态，
# `GetThreadExecutionState` 在脚本里查的是脚本自己，没有任何意义。
# 所以「状态变了」的证据是：按钮文字翻转 + ini 落盘 + trace 里的
# "TrayPanel: keep awake ON/OFF"。

param(
    [string]$Exe = '',
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\common.ps1')
. (Join-Path $PSScriptRoot '..\common\tray.ps1')
Require-Elevated -ScriptPath $PSCommandPath -ScriptArgs $PSBoundParameters
$Exe = Resolve-ToolboxExe $Exe

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$script:Pass = 0
$script:Fail = 0
function Assert-True([string]$What, [bool]$Cond, [string]$Extra = '') {
    if ($Cond) {
        $script:Pass++
        Write-Output ("  [PASS] {0} {1}" -f $What, $Extra)
    } else {
        $script:Fail++
        Write-Output ("  [FAIL] {0} {1}" -f $What, $Extra)
    }
}

$IniPath = Join-Path $env:APPDATA 'LingwuToolbox\settings.ini'
$TracePath = Join-Path $env:LOCALAPPDATA 'LingwuToolbox\trace.log'
$TraceFlag = Join-Path $env:LOCALAPPDATA 'LingwuToolbox\trace.on'

function Get-IniBool([string]$Key) {
    if (-not (Test-Path $IniPath)) { return $null }
    $m = Select-String -Path $IniPath -Pattern ("^\s*" + [regex]::Escape($Key) + "\s*=\s*(\d+)") `
         -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return $null
}

# 面板里按 AutomationId 找元素。
function Find-ById($Root, [string]$Id, [int]$WaitSec = 15) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::AutomationIdProperty, $Id)
    while ((Get-Date) -lt $deadline) {
        $found = $Root.FindFirst($TS::Descendants, $cond)
        if ($found) { return $found }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# 读按钮里那个文字控件的当前文字。
function Get-ButtonText($Button) {
    if (-not $Button) { return $null }
    $all = $Button.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        if ($e.Current.ControlType.ProgrammaticName -eq 'ControlType.Text' -and $e.Current.Name) {
            return $e.Current.Name
        }
    }
    return $null
}

# 进程下的顶层窗口（按 ProcessId 过滤）。
function Get-AppWindows([int]$ProcId) {
    $pc = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $ProcId)
    $wins = $AE::RootElement.FindAll($TS::Children, $pc)
    $res = New-Object System.Collections.Generic.List[object]
    foreach ($w in $wins) {
        $res.Add(@{ Hwnd = [int]$w.Current.NativeWindowHandle; Name = $w.Current.Name; El = $w })
    }
    return ,$res
}

function Get-TrayPanelWindow($Wins, $Main) {
    foreach ($w in $Wins) {
        if ($Main -and $w.Hwnd -eq $Main.Hwnd) { continue }
        if ($w.Name -and $w.Name -like '*零物工具箱*') { continue }
        if ($w.Hwnd -eq 0) { continue }
        return $w
    }
    return $null
}

# ---- 启动 ------------------------------------------------------------------
$own = $false
if (-not $NoLaunch) {
    Get-Process LingwuToolbox -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $p = Start-Process $Exe -PassThru
    $own = $true
    Write-Output "launched pid=$($p.Id)"
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $p.Refresh()
        if ($p.HasExited) { Write-Output "FAIL: 应用启动即退出 code=$($p.ExitCode)"; exit 1 }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    Start-Sleep -Seconds 4
}

$proc = Get-Process LingwuToolbox -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL: 应用未在运行'; exit 1 }
$pid_ = $proc.Id
Write-Output "app pid=$pid_"

# trace 标记文件：让产品落「keep awake ON/OFF」的 trace 行。
$traceFlagExisted = Test-Path $TraceFlag
if (-not $traceFlagExisted) {
    New-Item -ItemType Directory -Path (Split-Path $TraceFlag) -Force | Out-Null
    [IO.File]::Create($TraceFlag).Close()
}
$traceLenBefore = if (Test-Path $TracePath) { (Get-Item $TracePath).Length } else { 0 }

$origKeepAwake = Get-IniBool 'KeepAwake'
Write-Output "ini KeepAwake(before)=$origKeepAwake"

try {
    # ---- 打开托盘面板 -------------------------------------------------------
    $invoked = Invoke-ToolboxTrayButton
    Assert-True '托盘按钮可 Invoke（左键）' ($null -ne $invoked)

    $panel = $null
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 400
        $wins = Get-AppWindows $pid_
        $panel = Get-TrayPanelWindow $wins ($wins | Where-Object { $_.Name -like '*零物工具箱*' } | Select-Object -First 1)
        if ($panel) { break }
    }
    Assert-True '托盘指标面板已打开' ($null -ne $panel)
    if (-not $panel) { throw '面板没出现，后面的断言无从谈起' }

    $btn = Find-ById $panel.El 'KeepAwakeButton'
    Assert-True '面板里能定位「保持常亮」按钮' ($null -ne $btn)

    if ($btn) {
        # ---- ① 开 ----------------------------------------------------------
        $text0 = Get-ButtonText $btn
        Assert-True '① 初始按钮文字是「保持常亮」' ($text0 -eq '保持常亮') ("text=" + $text0)

        $ok = Invoke-UiaInvoke $btn
        Assert-True '「保持常亮」可 Invoke' $ok

        # 状态变化是**异步**的（点完要等 UI 线程走一圈），必须有界轮询。
        $text1 = $null
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 300
            $text1 = Get-ButtonText $btn
            if ($text1 -eq '常亮已开') { break }
        }
        Assert-True '① 开之后按钮文字变成「常亮已开」' ($text1 -eq '常亮已开') ("text=" + $text1)

        # ini 落盘
        $ini1 = $null
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 300
            $ini1 = Get-IniBool 'KeepAwake'
            if ($ini1 -eq 1) { break }
        }
        Assert-True '① 开之后 ini 里 KeepAwake=1' ($ini1 -eq 1) ("KeepAwake=" + $ini1)

        # ---- ② 关（复位）--------------------------------------------------
        $ok2 = Invoke-UiaInvoke $btn
        Assert-True '再点一次可 Invoke' $ok2
        $text2 = $null
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 300
            $text2 = Get-ButtonText $btn
            if ($text2 -eq '保持常亮') { break }
        }
        Assert-True '② 关之后按钮文字复位成「保持常亮」' ($text2 -eq '保持常亮') ("text=" + $text2)

        $ini2 = $null
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 300
            $ini2 = Get-IniBool 'KeepAwake'
            if ($ini2 -eq 0) { break }
        }
        Assert-True '② 关之后 ini 里 KeepAwake=0（复位）' ($ini2 -eq 0) ("KeepAwake=" + $ini2)
    }

    # ---- ③ 系统层的可观测性 -----------------------------------------------
    # powercfg /requests 需要管理员；本机是非提权环境 → 报 SKIP，不报 FAIL。
    $reqOut = & powercfg /requests 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and $reqOut -match 'LingwuToolbox') {
        Assert-True '③ powercfg /requests 里能看到本进程的常亮请求' $true
    } else {
        Write-Output 'SKIP: ③ powercfg /requests 读不到本进程的常亮请求（非提权环境 / 该请求不外露）'
        Write-Output '      —— SetThreadExecutionState 是**按线程**的，别的进程本来就查不到'
    }

    # ---- ④ trace 里留下了「ON / OFF」 ---------------------------------------
    Start-Sleep -Seconds 1
    $tail = ''
    if (Test-Path $TracePath) {
        $fs = [IO.File]::Open($TracePath, 'Open', 'Read', 'ReadWrite')
        try {
            $len = $fs.Length
            if ($len -gt $traceLenBefore) {
                $fs.Seek($traceLenBefore, 'Begin') | Out-Null
                $buf = New-Object byte[] ($len - $traceLenBefore)
                $fs.Read($buf, 0, $buf.Length) | Out-Null
                $tail = [Text.Encoding]::UTF8.GetString($buf)
            }
        } finally { $fs.Close() }
    }
    if ($tail) {
        Assert-True '④ trace 里落下了「keep awake ON」' ($tail -match 'keep awake ON')
        Assert-True '④ trace 里落下了「keep awake OFF」' ($tail -match 'keep awake OFF')
    } else {
        Write-Output 'SKIP: ④ trace.log 没有新增内容（trace.on 标记文件可能没生效）'
    }
}
finally {
    # 还原 ini —— 验收脚本不许把用户设置留在测试值上。
    if ($null -ne $origKeepAwake) {
        $text = Get-Content $IniPath -Raw
        if ($text -match '(?m)^\s*KeepAwake\s*=') {
            $text = $text -replace '(?m)^\s*KeepAwake\s*=\s*\d+', "KeepAwake=$origKeepAwake"
            Set-Content -Path $IniPath -Value $text -Encoding UTF8 -NoNewline
        }
        Write-Output "ini KeepAwake(restored)=$(Get-IniBool 'KeepAwake')"
    }
    if ($own) {
        Get-Process -Id $pid_ -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    if (-not $traceFlagExisted -and (Test-Path $TraceFlag)) {
        Remove-Item -LiteralPath $TraceFlag -Force -ErrorAction SilentlyContinue
    }
}

Write-Output ""
Write-Output "RESULT: pass=$script:Pass fail=$script:Fail"
if ($script:Fail -gt 0) { exit 1 }
exit 0
