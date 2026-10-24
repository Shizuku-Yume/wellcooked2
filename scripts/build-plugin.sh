#!/usr/bin/env bash
# 编译冒烟插件并拷进游戏目录的 BepInEx/plugins/。
#
# 用法: bash scripts/build-plugin.sh
#   GAME_DIR   游戏目录
#   DOTNET     dotnet 可执行文件，默认 ~/.dotnet/dotnet（本机只有用户级安装）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib.sh"
resolve_game_dir
GAME="$GAME_DIR"
DOTNET="${DOTNET:-$HOME/.dotnet/dotnet}"
PLUGIN="WellCooked.Smoke"

if [ ! -x "$DOTNET" ]; then
    DOTNET="$(command -v dotnet || true)"
fi
if [ -z "$DOTNET" ] || [ ! -x "$DOTNET" ]; then
    echo "找不到 dotnet；装 .NET SDK 或用 DOTNET=<路径> 指定。" >&2
    exit 1
fi

"$DOTNET" build "$ROOT/plugin/${PLUGIN}.csproj" -c Release -p:GameDir="$GAME" "$@"

mkdir -p "$GAME/BepInEx/plugins"
cp -v "$ROOT/plugin/bin/Release/net35/${PLUGIN}.dll" "$GAME/BepInEx/plugins/"
