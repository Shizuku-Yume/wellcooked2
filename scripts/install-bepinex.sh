#!/usr/bin/env bash
# 把 BepInEx 5.4.x 装进《胡闹厨房2》的游戏目录。幂等：重跑覆盖同名文件。
#
# 用法: bash scripts/install-bepinex.sh
#   GAME_DIR          游戏目录，默认本机 Fedora 的 Steam 库路径
#   BEPINEX_VERSION   默认 5.4.23.5
#   BEPINEX_SHA256    覆盖内置的发行包校验和（换版本或换架构时用）
#
# 架构从游戏 exe 的 PE 头读出来，不猜：本机这份 Overcooked2.exe 是 32 位（PE32/x86），
# 装错架构的 doorstop（winhttp.dll）不会被加载。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

resolve_game_dir
GAME="$GAME_DIR"
VERSION="${BEPINEX_VERSION:-5.4.23.5}"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/wellcooked"

if [ ! -f "$GAME/Overcooked2.exe" ]; then
    echo "找不到 Overcooked2.exe，这不像游戏目录: $GAME" >&2
    exit 1
fi

machine="$(python3 - "$GAME/Overcooked2.exe" <<'PY'
import struct, sys

try:
    with open(sys.argv[1], 'rb') as f:
        head = f.read(0x400)
    pe = struct.unpack_from('<I', head, 0x3c)[0]
    machine = struct.unpack_from('<H', head, pe + 4)[0]
except (OSError, struct.error, IndexError):
    print('不是可识别的 PE 文件: ' + sys.argv[1], file=sys.stderr)
    sys.exit(2)
print(machine)
PY
)"
case "$machine" in
    332)   ARCH=x86 ;;
    34404) ARCH=x64 ;;
    *)     echo "认不出 Overcooked2.exe 的架构（PE machine=$machine）" >&2; exit 1 ;;
esac

# 发行包的 sha256，取自 GitHub release 的 asset digest：
# gh api repos/BepInEx/BepInEx/releases/tags/v$VERSION -q '.assets[]|"\(.name) \(.digest)"'
expected_sha256() {
    case "${VERSION}/${ARCH}" in
        5.4.23.5/x86) echo 37651c79e40d6f909572a4f461ac25350bb3ef8fe7fbd29f1aa8791a33b84c82 ;;
        5.4.23.5/x64) echo 82f9878551030f54657792c0740d9d51a09500eeae1fba21106b0c441e6732c4 ;;
        *)            echo "" ;;
    esac
}
EXPECTED="${BEPINEX_SHA256:-$(expected_sha256)}"
if [ -z "$EXPECTED" ]; then
    echo "BepInEx $VERSION ($ARCH) 没有内置校验和，用 BEPINEX_SHA256=<sha256> 指定。" >&2
    exit 1
fi

ZIP="$CACHE/BepInEx_win_${ARCH}_${VERSION}.zip"
URL="https://github.com/BepInEx/BepInEx/releases/download/v${VERSION}/BepInEx_win_${ARCH}_${VERSION}.zip"

# 形状检查：包里必须有 winhttp.dll（doorstop 注入点）和 BepInEx/core
zip_ok() {
    local listing
    listing="$(unzip -l "$1" 2>/dev/null)" || return 1
    case "$listing" in *winhttp.dll*) ;; *) return 1 ;; esac
    case "$listing" in *"BepInEx/core/BepInEx.dll"*) return 0 ;; *) return 1 ;; esac
}

# 校验和：doorstop 会在游戏进程里执行，下载物必须对得上发行包的摘要
sha_ok() {
    [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$EXPECTED" ]
}

mkdir -p "$CACHE"

# 缓存要么完整要么没有：半个文件或改了内容的文件会被下一次运行当成有效缓存
if [ -f "$ZIP" ] && { ! zip_ok "$ZIP" || ! sha_ok "$ZIP"; }; then
    echo "缓存包不完整或校验和不符，重新下载: $ZIP" >&2
    rm -f "$ZIP"
fi

if [ ! -f "$ZIP" ]; then
    echo "下载 $URL"
    part="$ZIP.part"
    rm -f "$part"
    # 先写临时文件，形状和校验和都通过再改名，避免中断留下半个包
    curl -fL --retry 3 -o "$part" "$URL"
    if ! zip_ok "$part"; then
        rm -f "$part"
        echo "下载的包不完整，已丢弃" >&2
        exit 1
    fi
    if ! sha_ok "$part"; then
        rm -f "$part"
        echo "下载的包 sha256 不符（期望 $EXPECTED），已丢弃" >&2
        exit 1
    fi
    mv -f "$part" "$ZIP"
fi

# 换架构时先清掉上一次的 doorstop 文件，避免留下错误架构的 winhttp.dll
rm -f "$GAME/winhttp.dll"
unzip -o -q "$ZIP" -d "$GAME"

echo "BepInEx $VERSION ($ARCH) 已部署到: $GAME"
ls -1 "$GAME" | grep -E '^(winhttp\.dll|doorstop_config\.ini)$'
ls -1 "$GAME/BepInEx" | head -5
