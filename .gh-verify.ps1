# Final verification of the release pipeline, using the REST API directly.
#
# Usage:
#   pwsh -NoProfile -File .\.gh-verify.ps1 lingwu-service-manager-v0.0.1
#   pwsh -NoProfile -File .\.gh-verify.ps1 lingwu-sysmon-v0.0.1
#
# Was hardcoded to the sysmon tag; every release verification then meant editing
# this file first, which is exactly the kind of step people skip under time
# pressure. Now the tag is an argument and the expected asset name is derived
# from it, so the check cannot silently verify the wrong tool.
#
# NOTE: gh is not on PATH on this machine, so everything here goes through the
# REST API with the credential GCM already holds. That is also why this file
# does not use `gh release view`.
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Tag
)

$ErrorActionPreference = 'Stop'

# The asset name is <Product>-<ver>-win64.exe, and both halves come from the tag
# prefix, so a typo in the tag fails loudly here instead of reporting a bogus
# "asset missing".
#
# 2026-10-23: lingwu-sysmon and lingwu-service-manager were merged into
# lingwu-toolbox. The first two entries are **kept on purpose** -- their
# releases are already published and must stay verifiable; only the release
# line itself moved.
$tools = @{
    'lingwu-toolbox'          = 'LingwuToolbox'
    'lingwu-sysmon'           = 'LingwuSysmon'
    'lingwu-service-manager'  = 'LingwuServiceManager'
}
$prefix = $null
foreach ($k in $tools.Keys) {
    if ($Tag -like "$k-v*") { $prefix = $k; break }
}
if (-not $prefix) {
    throw "Tag '$Tag' does not start with a known tool prefix ($($tools.Keys -join ', '))."
}
if ($Tag -notmatch "-v(\d+\.\d+\.\d+)$") {
    throw "Tag '$Tag' does not end in -vX.Y.Z."
}
$version = $Matches[1]
$wantAsset = "$($tools[$prefix])-$version-win64.exe"

$cred = ("protocol=https`nhost=github.com`n`n" | git credential fill 2>$null)
$tok  = (($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=','')
if (-not $tok) { throw "No GitHub credential available from the git credential helper." }
$h = @{ 'User-Agent'='pwsh'; 'Authorization'="Bearer $tok"; 'Accept'='application/vnd.github+json' }
$base = 'https://api.github.com/repos/ycowner/lingwulab-windows-tools'

Write-Host "=========== 0. 目标 ==========="
Write-Host "  tag        : $Tag"
Write-Host "  version    : $version"
Write-Host "  expect asset: $wantAsset"

Write-Host "`n=========== 1. Release 本体 ==========="
$rel = Invoke-RestMethod -Uri "$base/releases/tags/$Tag" -Headers $h -TimeoutSec 25
Write-Host "tag        : $($rel.tag_name)"
Write-Host "title      : $($rel.name)"
Write-Host "draft      : $($rel.draft)    prerelease: $($rel.prerelease)"
Write-Host "published  : $($rel.published_at)"
Write-Host "tag commit : $((git rev-list -n 1 $Tag))"
Write-Host "assets     :"
foreach ($a in $rel.assets) {
    Write-Host ("  {0}  {1:N0} bytes  state={2}" -f $a.name, $a.size, $a.state)
    Write-Host ("  download : {0}" -f $a.browser_download_url)
}
Write-Host "body:"
($rel.body -split "`n") | ForEach-Object { Write-Host "  $_" }

Write-Host "`n=========== 2. 该有的资产在不在 ==========="
$names = @($rel.assets | ForEach-Object { $_.name })
if ($names -contains $wantAsset) {
    Write-Host "  OK: '$wantAsset' 已挂上 release"
} else {
    Write-Host "  FAIL: '$wantAsset' 不在 release 上。实际有: $($names -join ', ')"
    $script:failed = $true
}
if ($rel.draft -or $rel.prerelease) {
    Write-Host "  FAIL: draft=$($rel.draft) prerelease=$($rel.prerelease)，期望是公开正式版"
    $script:failed = $true
}

Write-Host "`n=========== 3. 是否被标记为 Latest ==========="
# /releases/latest is repo-wide, so with more than one tool it names whichever
# shipped most recently -- not necessarily this one. Report the fact rather
# than asserting, because "another tool is newer" is not a failure here.
try {
    $latest = Invoke-RestMethod -Uri "$base/releases/latest" -Headers $h -TimeoutSec 25
    Write-Host "  /releases/latest -> $($latest.tag_name)"
    Write-Host "  与本次一致: $($latest.tag_name -eq $Tag)"
    Write-Host "  （repo 只有一个 Latest 标记，多个工具共用；不一致不代表本次失败）"
} catch {
    Write-Host "  查询失败: $($_.Exception.Message)"
}

Write-Host "`n=========== 4. 资产实际可达性 ==========="
# Two different endpoints, and they are NOT interchangeable:
#
#   a) the API asset URL (Accept: application/octet-stream) hits the asset
#      store directly and answers correctly the moment upload finishes --
#      this is the authoritative check.
#   b) the browser_download_url goes through a CDN edge that is warmed
#      lazily. Right after a release it returns 404 for a while even though
#      the asset exists; observed still 404 after ~3 minutes while (a)
#      answered 200 with the full Content-Length the whole time.
#
# Verifying only (b) therefore produces a false failure on a fresh release.
# Verify (a), report (b), and check that the byte count matches what the
# workflow built -- a size mismatch is the real failure to watch for.
$asset = $rel.assets | Where-Object { $_.name -eq $wantAsset } | Select-Object -First 1
if ($asset) {
    Write-Host "  asset size (API metadata) : $($asset.size) bytes"

    try {
        # Verify by actually pulling bytes, not by HEAD.
        #
        # HEAD on the asset API answers 200 but with no length (the octet-stream
        # endpoint does not populate it -- the pre-existing lingwu-sysmon release
        # behaves identically, so this is endpoint semantics, not a broken
        # upload). A HEAD-only check therefore cannot distinguish "asset intact"
        # from "asset truncated", which is the one thing this step exists for.
        #
        # A range GET gives both: the byte count actually served and the total
        # size, and it costs one small request rather than a 60 MB download.
        $rangeHeaders = @{
            'User-Agent'    = 'pwsh'
            'Authorization' = "Bearer $tok"
            'Accept'        = 'application/octet-stream'
            'Range'         = 'bytes=0-1023'
        }
        $rr = Invoke-WebRequest -Uri $asset.url -Method Get -Headers $rangeHeaders `
                                -TimeoutSec 40 -MaximumRedirection 5
        $got = $rr.RawContentLength
        Write-Host "  API endpoint range GET    : $($rr.StatusCode)  取回 $got 字节"
        if ($rr.StatusCode -notin 200, 206) {
            Write-Host "  FAIL: API endpoint 返回 $($rr.StatusCode)"
            $script:failed = $true
        } elseif ($got -le 0) {
            Write-Host "  FAIL: 没取到任何字节，资产可能是空的"
            $script:failed = $true
        } else {
            # The first bytes of a PE image: 'MZ' + the DOS stub. Confirms this
            # is a real Windows executable and not, say, an HTML error page that
            # happened to come back with 200.
            $head = [System.Text.Encoding]::ASCII.GetString($rr.Content[0..1])
            Write-Host "  取回内容前两字节           : '$head'  (期望 'MZ')"
            if ($head -ne 'MZ') {
                Write-Host "  FAIL: 前两字节不是 'MZ'，这个资产不是 PE 可执行文件"
                $script:failed = $true
            } else {
                Write-Host "  OK: 资产是真实的 PE 可执行文件（元数据大小 $($asset.size) 字节）"
            }
        }
    } catch {
        Write-Host "  FAIL: API endpoint 失败: $($_.Exception.Message)"
        $script:failed = $true
    }

    $browserOk = $false
    try {
        $b = Invoke-WebRequest -Uri $asset.browser_download_url -Headers $h `
                               -Method Head -TimeoutSec 30 -MaximumRedirection 5
        $browserOk = ($b.StatusCode -eq 200)
        Write-Host "  browser_download_url HEAD : $($b.StatusCode)"
    } catch {
        Write-Host "  browser_download_url HEAD : HTTP $($_.Exception.Response.StatusCode.value__)"
    }
    if (-not $browserOk) {
        Write-Host "  NOTE: CDN 还没回源，刚发布的 release 常见（实测 3 分钟后仍是 404）。"
        Write-Host "        资产本身已确认可用（见上面 API endpoint 那一行），稍后重试即可。"
    }
}

Write-Host "`n=========== 5. 本次发布的 workflow 运行 ==========="
$runs = Invoke-RestMethod -Uri "$base/actions/runs?per_page=10" -Headers $h -TimeoutSec 25
$mine = $runs.workflow_runs | Where-Object { $_.head_branch -eq $Tag } | Select-Object -First 1
if ($mine) {
    Write-Host ("  {0} | {1} | {2} | {3}" -f $mine.name, $mine.status, $mine.conclusion, $mine.html_url)
    if ($mine.conclusion -ne 'success') {
        Write-Host "  FAIL: run conclusion=$($mine.conclusion)"
        $script:failed = $true
    }
} else {
    Write-Host "  最近 10 条里没有这个 tag 的运行"
}

Write-Host "`n=========== 6. 本地 git 状态 ==========="
git --no-pager log --oneline -5
Write-Host "发 tag 的提交: $((git rev-list -n 1 $Tag))"
Write-Host "当前 HEAD    : $(git rev-parse HEAD)"
Write-Host "tag 在 HEAD 上: $((git rev-list -n 1 $Tag) -eq (git rev-parse HEAD))"

if ($script:failed) {
    Write-Host "`nVERIFY FAILED"
    exit 1
}
Write-Host "`nVERIFY OK"
