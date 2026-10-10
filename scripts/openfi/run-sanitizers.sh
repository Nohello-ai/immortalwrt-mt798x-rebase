#!/bin/sh
# shellcheck disable=SC2086
#
# 带地址/未定义行为消毒器跑一遍全部测试
#
# 为什么单独一个脚本:每个测试要链的源文件不一样,写进 Makefile 反而绕。
# 普通测试抓不到的问题(越界读写、use-after-free、内存泄漏、未定义行为)
# 靠这个兜底 —— 嵌入式设备上没有 valgrind,这是本机能做的最接近的检查。
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
L="$P/openfi-lib/src"
M="$P/luci-app-openfi-modem/src"
S="$P/luci-app-openfi-sms/src"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
INC="-I$L -Iopenfi-lib/tests -I$M -I$S"
pass=0; fail=0

run() {
	name=$1; shift
	if ! gcc -O1 $SAN -std=gnu99 $INC -o "/tmp/asan_$name" "$@" 2>/tmp/b.log; then
		printf "  (跳过 %-12s 编译失败)\n" "$name"; head -2 /tmp/b.log | sed 's/^/     /'; return
	fi
	if ASAN_OPTIONS=detect_leaks=1 "/tmp/asan_$name" >/tmp/o.log 2>&1; then
		printf "  ✓ %-12s 干净\n" "$name"; pass=$((pass+1))
	else
		printf "  ✗ %-12s 消毒器报错:\n" "$name"
		grep -E 'ERROR|SUMMARY|runtime error|leaked' /tmp/o.log | head -4 | sed 's/^/       /'
		fail=$((fail+1))
	fi
}

run json     $P/openfi-lib/tests/test_json.c $L/openfi_json.c
run uci      $P/openfi-lib/tests/test_uci.c $L/openfi_uci.c
run sysfs    $P/openfi-lib/tests/test_sysfs.c $L/openfi_sysfs.c
run at       $P/openfi-lib/tests/test_at.c $L/openfi_at.c
run curve    $P/openfi-lib/tests/test_curve.c $L/openfi_curve.c
run fanctl   $P/openfi-lib/tests/test_fanctl.c $L/openfi_fanctl.c $L/openfi_curve.c
run fanstate $P/openfi-lib/tests/test_fanstate.c $L/openfi_fanctl.c $L/openfi_curve.c $L/openfi_temp.c $L/openfi_sysfs.c
run led      $P/openfi-lib/tests/test_led.c $L/openfi_led.c $L/openfi_sysfs.c
run temp     $P/openfi-lib/tests/test_temp.c $L/openfi_temp.c $L/openfi_sysfs.c
run switch   $P/openfi-lib/tests/test_switch.c $L/openfi_switch.c $L/openfi_sysfs.c
run sms      $P/luci-app-openfi-sms/tests/test_sms.c $S/openfi_sms.c
run store    $P/luci-app-openfi-sms/tests/test_store.c $S/openfi_sms_store.c $L/openfi_json.c $L/openfi_log.c $L/openfi_sysfs.c $L/openfi_uci.c
run modem    $P/luci-app-openfi-modem/tests/test_modem.c $M/openfi_modem.c

echo
echo "  消毒器:通过 $pass 组,失败 $fail 组"
[ "$fail" -eq 0 ] || exit 1
