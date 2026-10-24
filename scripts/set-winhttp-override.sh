#!/usr/bin/env bash
# 让 Proton/Wine 优先加载游戏目录里的 winhttp.dll，也就是 BepInEx 的 doorstop。
#
# 为什么需要：Wine 默认优先用自带的 winhttp，BepInEx 的注入点就不会被加载
# （见 https://docs.bepinex.dev/master/articles/advanced/steam_interop.html）。
# 这个脚本把覆盖写进 Proton 前缀的注册表，等价于在 Steam 启动项里写
# WINEDLLOVERRIDES="winhttp.dll=n,b" %command%，但不用改 Steam 配置。
#
# 用法: bash scripts/set-winhttp-override.sh
#   APPID  Steam AppID，默认值在 scripts/lib.sh（Overcooked! 2）
#   STEAM_COMPAT_DATA_PATH  兼容数据目录（不是前缀目录），默认按 APPID 推出来
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

# Proton 把 STEAM_COMPAT_DATA_PATH 当作 compatdata/<appid>，前缀是它下面的 pfx/
COMPAT_DATA="${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/Steam/steamapps/compatdata/$APPID}"
REG="$COMPAT_DATA/pfx/user.reg"

if [ ! -f "$REG" ]; then
    echo "找不到前缀注册表: $REG" >&2
    echo "先让游戏在 Proton 下启动过一次，或设置 STEAM_COMPAT_DATA_PATH。" >&2
    exit 1
fi

if ! command -v pgrep >/dev/null 2>&1; then
    echo "缺少 pgrep，无法确认游戏是否在运行，不敢改注册表。" >&2
    exit 1
fi
# 0 = 游戏在跑（Wine 退出时会覆写注册表，改了也白改）；1 = 没在跑；其他 = 查询失败
rc=0
pgrep -f 'Overcooked2.exe' >/dev/null 2>&1 || rc=$?
case "$rc" in
    0) echo "游戏正在运行，先退出游戏再执行。" >&2; exit 1 ;;
    1) ;;
    *) echo "pgrep 查询失败（退出码 $rc），不敢继续改注册表。" >&2; exit 1 ;;
esac

if [ ! -f "$REG.wellcooked-backup" ]; then
    cp "$REG" "$REG.wellcooked-backup"
    echo "已备份: $REG.wellcooked-backup"
fi

python3 - "$REG" <<'PY'
import os, re, sys, tempfile

path = sys.argv[1]
section = r'[Software\\Wine\\DllOverrides]'
with open(path, encoding='utf-8', errors='surrogateescape') as f:
    lines = f.read().split('\n')

start = next((i for i, line in enumerate(lines) if line.startswith(section)), None)
entry = '"winhttp"="native,builtin"'

if start is None:
    lines += ['', section + ' 1700000000', entry]
    action = '新增'
else:
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
    hit = next((i for i in range(start + 1, end) if lines[i].startswith('"winhttp"=')), None)
    if hit is None:
        lines.insert(start + 1, entry)
        action = '新增'
    elif lines[hit] == entry:
        action = '已存在'
    else:
        lines[hit] = entry
        action = '覆盖'

# 同目录临时文件写完再原子替换：直接截断原文件的话，写到一半失败会毁掉整份注册表
directory = os.path.dirname(os.path.abspath(path))
fd, tmp = tempfile.mkstemp(dir=directory, prefix='.user.reg.')
try:
    with os.fdopen(fd, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write('\n'.join(lines))
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, os.stat(path).st_mode & 0o7777)
    os.replace(tmp, path)
except BaseException:
    os.unlink(tmp)
    raise

print(f'winhttp 覆盖{action}: {entry}')
PY

grep -A3 -F '[Software\\Wine\\DllOverrides]' "$REG" | head -5
