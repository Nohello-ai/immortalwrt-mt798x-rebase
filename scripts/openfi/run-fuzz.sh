#!/bin/sh
#
# 解析器模糊测试(配合 ASan/UBSan)
#
# 解析器最容易被畸形输入打穿,而那些输入真实世界一定会遇到:
# 信号差时的半截报文、模组固件差异、别人塞进来的乱数据。
# 这个脚本用"随机字节 + 从合法报文变异"两种方式喂它们,
# 配上地址消毒器 —— 任何越界/崩溃/未定义行为都会立刻暴露。
#
# 用法: sh scripts/run-fuzz.sh [轮数](默认 5000,日常够用;
#       想深挖就传 200000)
#
set -u
# ── 定位包目录 ──────────────────────────────────────────────
# 从脚本所在目录往上找,直到看见 openfi-lib/src(开发目录)
# 或者 package/openfi/openfi-lib/src(固件树)。
# 这样同一份脚本在开发目录和固件树里都能直接跑。
_d=$(cd "$(dirname "$0")" && pwd)
P=. 
while [ "$_d" != "/" ]; do
	if [ -d "$_d/openfi-lib/src" ]; then P="$_d"; break; fi
	if [ -d "$_d/package/openfi/openfi-lib/src" ]; then P="$_d/package/openfi"; break; fi
	_d=$(dirname "$_d")
done
if [ ! -d "$P/openfi-lib/src" ]; then
	echo "找不到包目录(既不是开发目录也不是固件树)"; exit 1
fi
cd "$P" || exit 1
ROUNDS=${1:-5000}
L="$P/openfi-lib/src"
S="$P/luci-app-openfi-sms/src"

echo "编译(带 ASan/UBSan)..."
if ! gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu99 \
        -I"$L" -I"$S" -o /tmp/fuzz_parsers \
        $P/openfi-lib/tests/fuzz_parsers.c "$S/openfi_sms.c" \
        "$L/openfi_at.c" "$L/openfi_switch.c" "$L/openfi_uci.c" \
        "$L/openfi_json.c" "$L/openfi_sysfs.c" 2>/tmp/fuzz-build.log; then
	echo "  编译失败:"; head -8 /tmp/fuzz-build.log | sed 's/^/    /'; exit 1
fi
echo "  完成"
echo
ASAN_OPTIONS=detect_leaks=0 /tmp/fuzz_parsers "$ROUNDS"
