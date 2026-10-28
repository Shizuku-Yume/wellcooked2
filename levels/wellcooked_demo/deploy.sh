#!/usr/bin/env bash
# Linux 侧部署：把仓库里**已经构建好**的自制关卡 bundle 拷进游戏目录。
#
# 为什么 Linux 不能“构建”：AssetBundle 必须由 Unity 2017.4 的 Windows 构建目标生成，
# 本机没有该模块，也没有可用的 Unity 许可证（见任务 13 的 Progress）。所以这里只做部署；
# 真正的构建在 Windows 侧跑 build-only.ps1，或者由 WELLCOOKED_BUILD_CMD 指定的命令
# （例如 ssh 到构建机）完成后把 9 个 bundle 写回本目录，再执行本脚本。
#
# 用法:
#   bash levels/wellcooked_demo/deploy.sh [--game-dir DIR]
#
# 环境变量:
#   WELLCOOKED_GAME_DIR  游戏安装目录；不设时自动探测 Steam 路径。
#
# 退出码: 0 部署成功；1 缺文件/找不到游戏目录/校验失败。
set -euo pipefail

SET_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GAME_DIR=${WELLCOOKED_GAME_DIR:-}
FILES=(
    info_wellcooked_demo
    s_wellcooked_demo_1
    s_wellcooked_demo_2
    s_wellcooked_demo_3
    s_wellcooked_demo_4
    s_wellcooked_show_sushi
    s_wellcooked_show_beach
    s_wellcooked_show_summer
    s_wellcooked_show_city
)

while [ $# -gt 0 ]; do
    case "$1" in
        --game-dir)
            GAME_DIR=${2:?--game-dir 需要目录}
            shift 2
            ;;
        *)
            echo "未知参数: $1" >&2
            exit 2
            ;;
    esac
done

if [ -z "$GAME_DIR" ]; then
    for candidate in \
        "$HOME/.local/share/Steam/steamapps/common/Overcooked! 2" \
        "$HOME/.steam/steam/steamapps/common/Overcooked! 2" \
        "/run/media/${USER:-shizuku}/File/Steam/steamapps/common/Overcooked! 2"; do
        if [ -d "$candidate/BepInEx/plugins" ]; then
            GAME_DIR=$candidate
            break
        fi
    done
fi
if [ -z "$GAME_DIR" ] || [ ! -d "$GAME_DIR/BepInEx/plugins" ]; then
    echo "找不到游戏目录；用 --game-dir 或 WELLCOOKED_GAME_DIR 指定（需要存在 BepInEx/plugins）" >&2
    exit 1
fi

DEST=$GAME_DIR/BepInEx/plugins/OC2DIYLevel/levels/wellcooked_demo

# 源文件是否齐全
for f in "${FILES[@]}"; do
    if [ ! -f "$SET_DIR/$f" ]; then
        echo "缺少 $SET_DIR/$f；先在 Windows 侧构建并拷回仓库" >&2
        exit 1
    fi
done

# 布局比 bundle 新时仍然部署（按钮要能用），但必须把“没有重建”说清楚。
newest_layout=0
oldest_bundle=0
for f in "$SET_DIR"/layout_*.tsv; do
    [ -f "$f" ] || continue
    t=$(stat -c %Y "$f")
    [ "$t" -gt "$newest_layout" ] && newest_layout=$t
done
for f in "${FILES[@]}"; do
    t=$(stat -c %Y "$SET_DIR/$f")
    if [ "$oldest_bundle" -eq 0 ] || [ "$t" -lt "$oldest_bundle" ]; then
        oldest_bundle=$t
    fi
done
if [ "$newest_layout" -gt "$oldest_bundle" ]; then
    echo "WARN: layout_*.tsv 比 bundle 新；Linux 不会重建 Unity AssetBundle，本次部署的是旧 bundle。" >&2
    echo "WARN: 要应用布局改动，请在 Windows 侧重新构建，或设置 WELLCOOKED_BUILD_CMD。" >&2
fi

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
    if [ -f "$DEST/$f" ]; then
        cp -p "$DEST/$f" "$DEST/$f.bak"
    fi
    cp -p "$SET_DIR/$f" "$DEST/$f"
    if ! cmp -s "$SET_DIR/$f" "$DEST/$f"; then
        echo "校验失败: $DEST/$f" >&2
        exit 1
    fi
    echo "  $f"
done

echo "已部署 ${#FILES[@]} 个文件到 $DEST"
echo "重启游戏后 OC2DIYLevel 才会重新扫描关卡集。"
