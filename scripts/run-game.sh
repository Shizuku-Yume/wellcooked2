#!/usr/bin/env bash
# 启动《胡闹厨房2》（Steam + Proton Experimental）。
#
# 用法: bash scripts/run-game.sh
#   APPID  默认 728880（Overcooked! 2）
#
# BepInEx 的 doorstop 靠游戏目录里的 winhttp.dll 注入。如果启动后没有
# BepInEx/LogOutput.log，在 Steam 的启动项里加 WINEDLLOVERRIDES="winhttp=n,b" %command%，
# 或用 scripts/set-winhttp-override.sh 把这个覆盖写进 Proton 前缀的注册表。
set -euo pipefail

APPID="${APPID:-728880}"

if ! pgrep -x steam >/dev/null 2>&1; then
    echo "Steam 没在运行，先启动 Steam。" >&2
    exit 1
fi

echo "启动 appid $APPID"
exec steam -applaunch "$APPID"
