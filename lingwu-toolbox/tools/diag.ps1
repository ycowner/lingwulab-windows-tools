param([string]$Exe, [int]$Seconds = 20)

Get-Process LingwuSysmon -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$trace = Join-Path $env:LOCALAPPDATA 'LingwuSysMon\trace.log'
if (Test-Path $trace) { Remove-Item $trace -Force }

$p = Start-Process $Exe -PassThru
Write-Output "launched pid=$($p.Id)"

for ($i = 1; $i -le $Seconds; $i++) {
    Start-Sleep -Seconds 1
    $live = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $live) {
        Write-Output ("t={0,2}s  PROCESS GONE" -f $i)
        break
    }
    $tl = if (Test-Path $trace) { (Get-Content $trace).Count } else { -1 }
    Write-Output ("t={0,2}s  alive hwnd={1,-10} mainWin={2,-10} priv={3,7:N1}MB ws={4,7:N1}MB handles={5,-5} traceLines={6}" -f `
        $i, $live.Handle, $live.MainWindowHandle, `
        ($live.PrivateMemorySize64 / 1MB), ($live.WorkingSet64 / 1MB), `
        $live.HandleCount, $tl)
}
Write-Output '--- final trace tail ---'
if (Test-Path $trace) { Get-Content $trace -Tail 12 }
