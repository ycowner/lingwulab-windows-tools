param(
    [string]$Exe,
    [int]$Seconds = 180,
    [string]$OutCsv = "$env:TEMP\lingsysmon_mem.csv"
)

# 采集策略：**全部写进数组，结束时一次性输出 + 落盘**。
#
# 为什么不逐秒 Write-Output：后台任务的输出是流式的，读取端一有新行就返回，
# 于是每次轮询只能推进几秒，180 秒的观测要几十次轮询。改成汇总输出后，
# 只需要等一次完成通知。
Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$p = Start-Process $Exe -PassThru
$samples = New-Object System.Collections.Generic.List[object]

for ($i = 1; $i -le $Seconds; $i++) {
    Start-Sleep -Seconds 1
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
if (-not $alive) {
    Write-Output 'PROCESS NEVER SAMPLED'
    exit 1
}

$first = $alive[0]
$last  = $alive[-1]
Write-Output ("samples={0}  span={1}s" -f $alive.Count, $last.t)
Write-Output ("first: t={0}s priv={1}MB ws={2}MB" -f $first.t, $first.priv, $first.ws)
Write-Output ("last : t={0}s priv={1}MB ws={2}MB handles={3}" -f $last.t, $last.priv, $last.ws, $last.h)
Write-Output ("peak priv={0}MB at t={1}s" -f ($alive | Measure-Object priv -Maximum).Maximum,
                                             ($alive | Sort-Object priv -Descending)[0].t)

# 只打印「相对上一采样变化 >= 1MB」的点，以及最后 5 个点 —— 阶跃才是重点。
Write-Output '--- steps >= 1MB and tail ---'
$prev = $null
foreach ($s in $alive) {
    if ($null -eq $prev) { Write-Output ("  t={0,3}s  priv={1,6}  ws={2,6}  h={3}" -f $s.t, $s.priv, $s.ws, $s.h) }
    elseif ([math]::Abs($s.priv - $prev.priv) -ge 1.0 -or [math]::Abs($s.ws - $prev.ws) -ge 1.0) {
        Write-Output ("  t={0,3}s  priv={1,6}  ws={2,6}  h={3}   <-- step {4:+#.#;-#.#}MB" -f `
            $s.t, $s.priv, $s.ws, $s.h, ($s.priv - $prev.priv))
    }
    $prev = $s
}
Write-Output '--- last 5 ---'
$alive | Select-Object -Last 5 | ForEach-Object {
    Write-Output ("  t={0,3}s  priv={1,6}  ws={2,6}  h={3}" -f $_.t, $_.priv, $_.ws, $_.h)
}
Write-Output "CSV=$OutCsv"
