#!/usr/bin/env bash
# 游戏目录与 Steam AppID 的唯一来源：部署、构建、启动、评估脚本都 source 这个文件。
# 换机器只改这里的 DEFAULT_GAME_DIR，或运行时用 GAME_DIR 覆盖；各脚本不再各抄一份。
#
# 直接运行 `bash scripts/lib.sh` 会走同一条解析规则并打印绝对路径（Python 工具和人工
# 诊断都用它），目录不存在时打印原因和修法并退出 1。

DEFAULT_GAME_DIR="/run/media/shizuku/File/Steam/steamapps/common/Overcooked! 2"
APPID="${APPID:-728880}"

# 把 GAME_DIR（缺省用 DEFAULT_GAME_DIR）检查并绝对化。成功时覆盖并导出 GAME_DIR，
# 子进程（run-game.sh、dotnet）看到的是同一个目录。
resolve_game_dir() {
    local dir="${GAME_DIR:-$DEFAULT_GAME_DIR}"

    if [ ! -d "$dir" ]; then
        echo "游戏目录不存在: $dir" >&2
        echo "  用 GAME_DIR=<路径> 指定，例如:" >&2
        echo "    GAME_DIR=\"/path/to/Steam/steamapps/common/Overcooked! 2\" bash scripts/install-bepinex.sh" >&2
        echo "  默认路径在 scripts/lib.sh 的 DEFAULT_GAME_DIR。" >&2
        return 1
    fi
    # 各脚本的调用者工作目录不同，后续的 cp/unzip/dotnet 又各按各的 cwd 解析相对路径；
    # 在这里统一成绝对路径，避免"装到一个目录、构建读另一个目录"。
    local resolved
    if ! resolved="$(cd "$dir" && pwd)"; then
        echo "无法进入游戏目录: $dir" >&2
        return 1
    fi
    GAME_DIR="$resolved"
    export GAME_DIR
}

# source 时只定义函数；直接执行时解析并打印，供 Python 工具复用同一份规则。
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    resolve_game_dir || exit 1
    printf '%s\n' "$GAME_DIR"
fi
