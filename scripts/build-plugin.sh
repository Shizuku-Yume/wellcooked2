#!/usr/bin/env bash
# 编译冒烟插件并拷进游戏目录的 BepInEx/plugins/。
#
# 用法: bash scripts/build-plugin.sh
#   GAME_DIR   游戏目录
#   DOTNET     dotnet 可执行文件，默认 ~/.dotnet/dotnet（本机只有用户级安装）
#   DEPLOY=0   只编译不拷：游戏开着时覆盖已加载的 DLL 不安全，由调用方比对后决定
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${GAME_DIR:-/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2}"
DOTNET="${DOTNET:-$HOME/.dotnet/dotnet}"
PLUGIN="WellCooked.Smoke"

if [ ! -d "$GAME" ]; then
    echo "游戏目录不存在: $GAME（用 GAME_DIR=<路径> 指定）" >&2
    exit 1
fi
# MSBuild 按项目目录解析 HintPath，shell 按调用者工作目录解析 cp；
# 传相对路径会让两边指向不同目录，所以先统一成绝对路径
GAME="$(cd "$GAME" && pwd)"

if [ ! -x "$DOTNET" ]; then
    DOTNET="$(command -v dotnet || true)"
fi
if [ -z "$DOTNET" ] || [ ! -x "$DOTNET" ]; then
    echo "找不到 dotnet；装 .NET SDK 或用 DOTNET=<路径> 指定。" >&2
    exit 1
fi

"$DOTNET" build "$ROOT/plugin/${PLUGIN}.csproj" -c Release -p:GameDir="$GAME" "$@"

if [ "${DEPLOY:-1}" = 1 ]; then
    mkdir -p "$GAME/BepInEx/plugins"
    cp -v "$ROOT/plugin/bin/Release/net35/${PLUGIN}.dll" "$GAME/BepInEx/plugins/"
fi
