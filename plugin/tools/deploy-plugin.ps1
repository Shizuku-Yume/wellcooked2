<#
.SYNOPSIS
    编译（可选）并部署 WellCooked.Plugin.dll 到《胡闹厨房2》的 BepInEx\plugins 目录。

.DESCRIPTION
    默认从 plugin\WellCooked.Plugin\bin\Debug\WellCooked.Plugin.dll 复制到
    $GameDir\BepInEx\plugins\WellCooked\。加 -Build 时先用工作区 .tools\dotnet8 编译。

.EXAMPLE
    .\deploy-plugin.ps1 -Build
    .\deploy-plugin.ps1 -GameDir "D:\EPIC\Games\Overcooked2"
#>
param(
    [string]$GameDir = "D:\EPIC\Games\Overcooked2",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

# 仓库根 = 本脚本上两级（plugin\tools -> plugin -> repo）
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projDir = Join-Path $repoRoot "plugin\WellCooked.Plugin"
$projFile = Join-Path $projDir "WellCooked.Plugin.csproj"
$dll = Join-Path $projDir ("bin\" + $Configuration + "\WellCooked.Plugin.dll")

Write-Host "== WellCooked: deploy plugin ==" -ForegroundColor Cyan
Write-Host "Repo    : $repoRoot"
Write-Host "Config  : $Configuration"

# ---- 1. 可选编译 ----
if ($Build) {
    $dotnet = Join-Path $repoRoot "..\.tools\dotnet8\dotnet.exe"
    if (-not (Test-Path -LiteralPath $dotnet)) { $dotnet = "dotnet" }
    Write-Host "Build   : $dotnet build -c $Configuration"
    & $dotnet build $projFile -c $Configuration
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }
}

if (-not (Test-Path -LiteralPath $dll)) {
    throw "找不到产物: $dll`n先编译：dotnet build `"$projFile`" -c $Configuration（或加 -Build 重跑本脚本）"
}

# ---- 2. 校验游戏目录与 BepInEx ----
if (-not (Test-Path -LiteralPath $GameDir)) { throw "游戏目录不存在: $GameDir" }
$bepinexCore = Join-Path $GameDir "BepInEx\core\BepInEx.dll"
if (-not (Test-Path -LiteralPath $bepinexCore)) {
    throw "游戏目录还没装 BepInEx（缺 BepInEx\core\BepInEx.dll）。先运行 .\install-bepinex.ps1"
}

# ---- 3. 复制 ----
$pluginDir = Join-Path $GameDir "BepInEx\plugins\WellCooked"
if (-not (Test-Path -LiteralPath $pluginDir)) {
    New-Item -ItemType Directory -Path $pluginDir | Out-Null
}
Copy-Item -LiteralPath $dll -Destination (Join-Path $pluginDir "WellCooked.Plugin.dll") -Force
$size = (Get-Item -LiteralPath $dll).Length
Write-Host ("Copied  : WellCooked.Plugin.dll ({0:N0} bytes) -> {1}" -f $size, $pluginDir)
Write-Host "部署完成。启动游戏后查看 BepInEx\LogOutput.log 中 'WellCooked' 相关日志。" -ForegroundColor Green
