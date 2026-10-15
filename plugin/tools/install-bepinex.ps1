<#
.SYNOPSIS
    把 BepInEx 5.4.23.3 (x64) 安装到《胡闹厨房2》游戏目录（兜底脚本，等价于手动解压覆盖）。

.DESCRIPTION
    优先从 -Source 指定的解压目录复制；找不到时尝试从 $env:TEMP 下的 *_x64_*.zip 解压；
    再找不到就提示手动下载。复制后校验 winhttp.dll / doorstop_config.ini / BepInEx\core\BepInEx.dll。

.EXAMPLE
    .\install-bepinex.ps1
    .\install-bepinex.ps1 -GameDir "D:\EPIC\Games\Overcooked2"
#>
param(
    [string]$GameDir = "D:\EPIC\Games\Overcooked2",
    [string]$Source = "$env:TEMP\bepinex_x64_extract"
)

$ErrorActionPreference = "Stop"

Write-Host "== WellCooked: install BepInEx x64 ==" -ForegroundColor Cyan
Write-Host "GameDir : $GameDir"
Write-Host "Source  : $Source"

# ---- 1. 校验游戏目录 ----
if (-not (Test-Path -LiteralPath $GameDir)) {
    throw "游戏目录不存在: $GameDir"
}
$exe = Get-ChildItem -LiteralPath $GameDir -Filter "*.exe" -File -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -match "Overcooked" } | Select-Object -First 1
if (-not $exe) {
    Write-Warning "游戏目录里没找到 Overcooked*.exe，请确认路径是否正确。"
} else {
    Write-Host ("Found game exe: " + $exe.Name)
}

# ---- 2. 准备来源（解压目录 / zip / 提示） ----
$neededInSource = @("winhttp.dll", "doorstop_config.ini", "BepInEx")
if (-not (Test-Path -LiteralPath $Source)) {
    # 官方 5.4.23.3 的 Windows x64 资产名是 BepInEx_win_x64_5.4.23.3.zip。
    # 用 *_x64_*.zip 而不是 BepInEx_x64_*.zip：后者匹配不到真实文件名，
    # 会让"下载成功却仍然报找不到来源"变成一个查不出来的死角。
    $zip = Get-ChildItem -Path $env:TEMP -Filter "*_x64_*.zip" -File -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($zip) {
        $Source = Join-Path $env:TEMP "bepinex_x64_extract"
        Write-Host "Extracting $($zip.FullName) -> $Source"
        if (Test-Path -LiteralPath $Source) { Remove-Item -LiteralPath $Source -Recurse -Force }
        Expand-Archive -LiteralPath $zip.FullName -DestinationPath $Source -Force
    } else {
        throw @"
没有可用的 BepInEx 安装源。
请任选其一：
  1) 手动下载 https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.3/BepInEx_win_x64_5.4.23.3.zip
     解压到 $Source 后重跑本脚本；
  2) 把 zip 放到 $env:TEMP 下（文件名形如 BepInEx_win_x64_5.4.23.3.zip）后重跑本脚本。
"@
    }
}
foreach ($n in $neededInSource) {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $n))) {
        throw "安装源不完整（缺 $n）: $Source"
    }
}

# ---- 3. 复制到游戏目录（不覆盖玩家存档/配置，只覆盖 BepInEx 自身文件） ----
$items = @("winhttp.dll", "doorstop_config.ini", "changelog.txt", ".doorstop_version", "BepInEx")
foreach ($item in $items) {
    $src = Join-Path $Source $item
    if (-not (Test-Path -LiteralPath $src)) { continue }
    $dst = Join-Path $GameDir $item
    if (Test-Path -LiteralPath $dst) {
        Write-Host "Overwrite: $item"
    } else {
        Write-Host "Install  : $item"
    }
    Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
}

# ---- 4. 校验关键文件 ----
$checks = @("winhttp.dll", "doorstop_config.ini", "BepInEx\core\BepInEx.dll")
$missing = @()
foreach ($c in $checks) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir $c))) { $missing += $c }
}
if ($missing.Count -gt 0) {
    throw ("安装校验失败，缺失: " + ($missing -join ", "))
}

# ---- 5. 确认 plugins 目录存在 ----
$pluginsDir = Join-Path $GameDir "BepInEx\plugins"
if (-not (Test-Path -LiteralPath $pluginsDir)) {
    New-Item -ItemType Directory -Path $pluginsDir | Out-Null
}

Write-Host "BepInEx x64 installed OK." -ForegroundColor Green
Write-Host "plugins dir: $pluginsDir"
Write-Host "下一步：.\deploy-plugin.ps1 部署 WellCooked.Plugin.dll，然后启动游戏。"
