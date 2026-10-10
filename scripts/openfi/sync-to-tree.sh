#!/bin/sh
#
# 把开发目录里的包同步进固件源码树
#
# 为什么要有这一步:
#   开发在 /root/1/openfi-packages-new(那里有本地测试、Makefile.test、e2e 脚本),
#   而固件要的是"能直接编"的形态 —— 放在固件仓库的 package/openfi/ 下。
#   两边职责不同,所以用这个脚本单向同步(开发目录 → 树),避免手抄漏文件。
#
# 用法: sh scripts/sync-to-tree.sh [固件树路径]
#
set -e

TREE=${1:-/root/1/openfi-25.12}
[ -d "$TREE/package" ] || { echo "找不到固件树: $TREE"; exit 1; }

SRC=$(cd "$(dirname "$0")/.." && pwd)
DST="$TREE/package/openfi"

echo "从 $SRC"
echo "到   $DST"
echo

mkdir -p "$DST"
for p in openfi-lib luci-app-openfi-fan luci-app-openfi-modem luci-app-openfi-sms; do
	[ -d "$SRC/$p" ] || { echo "  跳过 $p（不存在）"; continue; }
	# ★ 变量必须非空才允许 rm -rf —— 万一 DST 或 p 空掉,
	#   "rm -rf /$p" 就是删根目录。shellcheck 的 SC2115 指的就是这个。
	if [ -z "${DST:-}" ] || [ -z "${p:-}" ]; then
		echo "内部错误:路径为空,拒绝删除"; exit 1
	fi
	rm -rf "${DST:?}/${p:?}"
	cp -a "$SRC/$p" "$DST/"
	# 本地开发才用的东西不进固件源码树
	rm -f "$DST/$p/Makefile.test"
	rm -rf "$DST/$p/tests/.deps"
	printf "  ✅ %-24s %2d 个文件\n" "$p" "$(find "$DST/$p" -type f | wc -l)"
done

# 开发工具(测试/消毒器/模糊/变异脚本)也一起进树 ——
# 不然仓库里的包只有源码没有验证手段,别人拿到手没法自测。
DEV="$TREE/scripts/openfi"
mkdir -p "$DEV"
for f in run-sanitizers.sh run-fuzz.sh run-mutation.sh sync-to-tree.sh; do
	[ -f "$SRC/scripts/$f" ] && cp -a "$SRC/scripts/$f" "$DEV/" && echo "  ✅ scripts/openfi/$f"
done

echo
echo "同步完成。可以用 git -C $TREE status 看差异。"
