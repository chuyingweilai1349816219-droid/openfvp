# push.ps1 —— 把本地仓库推送到 GitHub（带代理自动检测）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\push.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\push.ps1 -Proxy http://127.0.0.1:7890
#   powershell -ExecutionPolicy Bypass -File scripts\push.ps1 -DryRun
#
# 做的事：
#   1. 把 remote 切成 HTTPS（SSH 在部分网络下不可用）
#   2. 先试直连 github.com:443；不通就扫描常见本地代理端口，找到就把 git 配成走它
#   3. 推送 main 与所有 tag
#
# 首次推送需要 GitHub Personal Access Token（不是账号密码），
# 生成地址：https://github.com/settings/tokens  权限只需勾 repo

param(
    [string]$Remote = 'https://github.com/chuyingweilai1349816219-droid/openfvp.git',
    [string]$Proxy  = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# git 可执行文件：优先 PATH，其次仓库自带的便携版
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if (-not $git) {
    $local = Join-Path (Split-Path $PSScriptRoot -Parent) '..\tools\mingit\cmd\git.exe'
    if (Test-Path $local) { $git = (Resolve-Path $local).Path }
}
if (-not $git) { throw '找不到 git：请安装 Git，或用 tools\mingit 里的便携版' }

$repo = Split-Path $PSScriptRoot -Parent
Push-Location $repo
try {
    Write-Host "== 仓库 ==" -ForegroundColor Cyan
    Write-Host "  $repo"
    & $git remote set-url origin $Remote
    Write-Host "  remote -> $Remote"

    # ---- 代理检测 ----
    function Test-Https([string]$p) {
        # 用 .NET 发 HEAD 请求：不会像原生 curl 那样把 stderr 变成 PowerShell 错误
        try {
            $req = [System.Net.HttpWebRequest]::Create('https://github.com')
            $req.Method = 'HEAD'
            $req.Timeout = 12000
            $req.AllowAutoRedirect = $false
            if ($p) {
                $req.Proxy = New-Object System.Net.WebProxy($p, $true)
            } else {
                $req.Proxy = $null   # 直连，不走系统代理
            }
            $resp = $req.GetResponse()
            $code = [int]$resp.StatusCode
            $resp.Close()
            return ($code -ge 200 -and $code -lt 400)
        } catch {
            return $false
        }
    }

    $use = $null
    if ($Proxy) {
        if (Test-Https $Proxy) { $use = $Proxy } else { Write-Host "  指定代理不可用：$Proxy" -ForegroundColor Yellow }
    }
    if (-not $use) {
        Write-Host "== 测试直连 github.com:443 ==" -ForegroundColor Cyan
        if (Test-Https '') { Write-Host "  直连可用" -ForegroundColor Green }
        else {
            Write-Host "  直连不通，扫描本地代理端口…" -ForegroundColor Yellow
            $candidates = @(7890, 7891, 7892, 7897, 10808, 10809, 1080, 8080, 2080, 20171)
            $listening = @()
            foreach ($port in $candidates) {
                $c = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
                     Where-Object { $_.LocalPort -eq $port }
                if ($c) { $listening += $port }
            }
            foreach ($port in $listening) {
                $p = "http://127.0.0.1:$port"
                Write-Host "  试 $p …" -NoNewline
                if (Test-Https $p) { $use = $p; Write-Host " 可用" -ForegroundColor Green; break }
                else { Write-Host " 不通" }
            }
            if (-not $use -and $listening.Count -eq 0) {
                Write-Host "  没有发现任何本地代理端口在监听。" -ForegroundColor Red
            }
        }
    }

    if ($use) {
        & $git config --global http.proxy  $use
        & $git config --global https.proxy $use
        Write-Host "  git 代理 -> $use" -ForegroundColor Green
    } else {
        Write-Host "  继续尝试直连（大概率会失败）" -ForegroundColor Yellow
    }

    # ---- 推送 ----
    Write-Host "== 待推送 ==" -ForegroundColor Cyan
    Write-Host ("  分支: " + (& $git branch --show-current))
    Write-Host ("  提交: " + ((& $git log --oneline -1)))
    Write-Host ("  tag : " + ((& $git tag) -join ', '))

    if ($DryRun) {
        Write-Host "`n[DryRun] 不执行推送。命令会是：" -ForegroundColor Cyan
        Write-Host "  $git push -u origin main --tags"
        return
    }

    Write-Host "`n== 推送 ==" -ForegroundColor Cyan
    Write-Host "  首推会要求 Username / Password：" -ForegroundColor DarkGray
    Write-Host "    Username = chuyingweilai1349816219-droid" -ForegroundColor DarkGray
    Write-Host "    Password = 你生成的 Personal Access Token" -ForegroundColor DarkGray
    & $git push -u origin main --tags
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n推送失败。常见原因：" -ForegroundColor Red
        Write-Host "  * 仓库还没在 GitHub 上创建 → https://github.com/new 建 openfvp（Public，不勾初始化）"
        Write-Host "  * Token 没勾 repo 权限，或复制时漏了字符"
        Write-Host "  * 代理软件没开 / 端口不是常见那几个 → 用 -Proxy http://127.0.0.1:端口 指定"
        exit 1
    }
    Write-Host "`n推送成功。仓库地址：$($Remote -replace '\.git$','')" -ForegroundColor Green
}
finally {
    Pop-Location
}