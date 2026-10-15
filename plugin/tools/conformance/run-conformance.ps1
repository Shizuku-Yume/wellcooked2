# run-conformance.ps1 —— 一致性测试的 Windows 半场：把对端从插件自己的协议代码编译出来，
# 再把运行交给 WSL。
#
#     powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1
#     powershell -NoProfile -File plugin/tools/conformance/run-conformance.ps1 -Port 12401 -Seconds 3
#
# 退出码：0 通过，1 双方不一致（看 WSL 半场的输出），3 前置条件不成立（没有编译器、
# 对端没编译出来、桥没起来）。
#
# 为什么分两半：这次运行横跨两个操作系统。
#   * 编译必须在这里做。工作目录是本检出时，csc.exe 无法经 WSL interop 驱动：路径不是
#     ASCII，交给它的每个源文件都打不开（报 "Source file '<仓库>\PluginPeer.cs' could
#     not be opened"），而同一条命令从 Windows 侧跑就干净通过。不拿别的东西替代它——
#     csc 不在就直接说明并停下；这里换成 Python 重写，整个测试的意义就没了。
#   * 运行在 WSL 里做（run-conformance.sh）：桥进程、worker stub、对端的退出码和桥日志
#     都归它管。
#
# 跨边界传的参数都是 ASCII 且不含空格：`wsl.exe -e bash <脚本> <参数>` 是唯一能原样穿过去的
# 调用形式；凡是要引号的东西，都会被 wsl.exe 重拼 Linux 命令行时静默搞坏。

[CmdletBinding()]
param(
    [int]$Port = 12401,
    [double]$Seconds = 3.0,
    [string]$RequireKinds = 'move,stop',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$Csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$PeerExe = Join-Path $RepoRoot 'plugin\tools\conformance\bin\PluginPeer.exe'
$Sources = @(
    (Join-Path $RepoRoot 'plugin\tools\conformance\PluginPeer.cs'),
    (Join-Path $RepoRoot 'plugin\WellCooked.Plugin\Protocol\Messages.cs'),
    (Join-Path $RepoRoot 'plugin\WellCooked.Plugin\Net\TcpLink.cs')
)
$CscLog = Join-Path $RepoRoot 'logs\conformance-csc.out'

if (-not $SkipBuild) {
    Write-Output '== 用 .NET Framework 编译器编译对端 =='
    if (-not (Test-Path -LiteralPath $Csc)) {
        Write-Output "在 $Csc 找不到 csc"
        Write-Output '需要 Windows 自带的 .NET Framework 4.x 编译器。'
        Write-Output '不替换成别的编译器：为这个测试另写的对端，'
        Write-Output '证明不了它本该检验的插件代码。'
        exit 3
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $PeerExe) | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $CscLog) | Out-Null
    Remove-Item -LiteralPath $PeerExe -ErrorAction SilentlyContinue   # 绝不测一个过期的二进制

    $cscArgs = @(
        '/nologo', '/target:exe', '/warn:4', '/preferreduilang:en-US',
        "/out:$PeerExe"
    ) + $Sources

    # 先打印再执行：这样即使编译器没有任何输出，证据里也有确切的命令行。
    Write-Output ("csc: `"$Csc`" " + (($cscArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '))

    # csc 用控制台代码页写诊断信息，按 UTF-8 读回来，免得非 ASCII 的报错被悄悄换成 '?'。
    $prev = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        $cscOutput = & $Csc $cscArgs 2>&1
        $cscExit = $LASTEXITCODE
    } finally {
        [Console]::OutputEncoding = $prev
    }
    # 无论编译器有没有说话都要重写日志：把空数组管进 Set-Content 并不会截断文件，那会让上一轮
    # 的错误和这一次的成功退出码并排躺着。UTF-8 不带 BOM，于是「编译器没说话」就是一个
    # 0 字节文件，而不是孤零零一个 BOM。
    $cscText = ($cscOutput -join [Environment]::NewLine)
    [System.IO.File]::WriteAllText($CscLog, $cscText, (New-Object System.Text.UTF8Encoding($false)))

    if ($cscOutput) { $cscOutput | ForEach-Object { Write-Output "  $_" } }
    Write-Output "csc 退出码：$cscExit"
    if ($cscExit -ne 0) {
        Write-Output '对端没编译出来，就没有东西可测。'
        Write-Output '拿旧二进制去跑，得到的证据属于那个二进制，不属于这些源码。'
        exit 3
    }
    Write-Output "已编译：$PeerExe（$((Get-Item -LiteralPath $PeerExe).Length) 字节）"
    Write-Output ''
}

Write-Output '== 把运行交给 WSL（桥 + worker stub + 对端）=='
& wsl.exe -e bash plugin/tools/conformance/run-conformance.sh $Port $Seconds $RequireKinds
$rc = $LASTEXITCODE
Write-Output ''
Write-Output "run-conformance.sh 退出码：$rc"
exit $rc
