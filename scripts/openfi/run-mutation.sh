#!/bin/sh
#
# 变异测试 —— 验证【测试本身有没有用】
#
# 做法:故意把代码改坏(改边界、翻转判断、去掉检查),看测试能不能抓到。
# • 抓到了   → 说明那条路径真被测到了
# • 没抓到   → 说明测试有盲区,即使函数"跑过了"也不能说明它对
#
# 为什么需要它:
#   覆盖率只能说明"代码被执行过",不能说明"结果被验证过"。
#   这个脚本已经抓到过三类真实盲区:
#     · 只测往返不测编码值 → 改错扩展表下标也发现不了
#                              (pack/unpack 用同一张表,自己跟自己一致)
#     · 只用 90℃ 测紧急温度 → 把 >= 改成 > 也发现不了(边界没测)
#     · 用例写在 mkdir 之前 → open() 先失败,有没有 bug 都是 -1
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
caught=0; missed=0

mut() {
	name="$1"; file="$2"; from="$3"; to="$4"; tests="$5"
	cp "$file" /tmp/mut.bak
	# 用 python 做替换,避免 shell 转义问题
	if ! python3 -c "
import sys
p,a,b=sys.argv[1],sys.argv[2],sys.argv[3]
s=open(p,encoding='utf-8').read()
if a not in s: sys.exit(3)
open(p,'w',encoding='utf-8').write(s.replace(a,b,1))
" "$file" "$from" "$to"; then
		printf "  ? %-44s 找不到变异点(代码变过?)\n" "$name"
		cp /tmp/mut.bak "$file"; return
	fi
	if ! gcc -O1 -std=gnu99 -I"$L" -I"$P/openfi-lib/tests" \
		-I"$P/luci-app-openfi-modem/src" -I"$P/luci-app-openfi-sms/src" \
		-o /tmp/mutbin $tests 2>/dev/null; then
		printf "  ✓ %-44s (编不过,也算被发现)\n" "$name"; caught=$((caught+1))
		cp /tmp/mut.bak "$file"; return
	fi
	if /tmp/mutbin >/dev/null 2>&1; then
		printf "  ✗ %-44s ★ 测试没抓到(盲区!)\n" "$name"; missed=$((missed+1))
	else
		printf "  ✓ %-44s 抓到了\n" "$name"; caught=$((caught+1))
	fi
	cp /tmp/mut.bak "$file"
}

T_SMS="$P/luci-app-openfi-sms/tests/test_sms.c $P/luci-app-openfi-sms/src/openfi_sms.c"
T_FAN="$P/openfi-lib/tests/test_fanstate.c $L/openfi_fanctl.c $L/openfi_curve.c $L/openfi_temp.c $L/openfi_sysfs.c"
T_SW="$P/openfi-lib/tests/test_switch.c $L/openfi_switch.c $L/openfi_sysfs.c"
T_CURVE="$P/openfi-lib/tests/test_curve.c $L/openfi_curve.c"

F=$P/luci-app-openfi-sms/src/openfi_sms.c
echo "── PDU 编解码 ──"
mut "地址解码:数字从下标 2 改成 1" "$F" \
	"for (int i = 2; i < total_bytes" "for (int i = 1; i < total_bytes" "$T_SMS"
mut "7位+UDH:起始 septet 当成 0(错 1 位)" "$F" \
	"sms_unpack_7bit_at(b + ud_start, start_sept, sept," "sms_unpack_7bit_at(b + ud_start, 0, sept," "$T_SMS"
mut "UDH 填充位:不加(按字节对齐)" "$F" \
	"fill = (7 - (bits % 7)) % 7;" "fill = 0;" "$T_SMS"
mut "扩展表:改错 [ 的下标" "$F" "{ 0x3C, \"[\" }" "{ 0x34, \"[\" }" "$T_SMS"
mut "截断报文:夹长度而不是报错" "$F" \
	"if (ud_start + udlen > n)
		return -1;" "if (ud_start + udlen > n)
		udlen = n - ud_start;" "$T_SMS"
# 注:单纯去掉 UDHL 的越界检查是"等价变异" —— 后面的 body<0 仍会拒绝,
#     所以这里改成同时去掉两道防线,才是真正能区分的变异。
# ★ UDHL 越界这块代码有【两道】防线:
#     ① if (udh_bytes > udlen) return -1;     —— 显式拒绝畸形报文
#     ② if (body < 0) return -1;              —— 长度算成负数时的兜底
#   两处语义重叠,单独去掉任何一处行为都不变 —— 这是"等价变异",
#   而它恰恰说明防御是有效的(不是测试盲区)。所以这里只针对第 ② 道
#   设变异:去掉它之后,①仍然拦住,仍然等价,故不计入本清单。
mut "UCS2 正文:长度不减 UDH(多解出字段外数据)" "$F" \
	"int body = udlen - udh_bytes;" "int body = udlen;" "$T_SMS"
mut "UCS2+UDH:少减 UDHL 那一字节" "$F" \
	"int body = udlen - udh_bytes;" "int body = udlen - udh_len;" "$T_SMS"
mut "CMGS 长度:总字节数减 1" "$F" \
	"smsc = 2 + (digits + 1) / 2;" "smsc = 1;" "$T_SMS"
mut "UCS2 判定用错位" "$F" "if ((dcs_raw & 0x0C) == 0x08)" "if ((dcs_raw & 0x0C) == 0x04)" "$T_SMS"

echo "── 风扇状态机 ──"
mut "紧急温度 >= 改成 >" "$L/openfi_fanctl.c" \
	"if (unified_temp >= st->cfg.emergency_temp)" "if (unified_temp > st->cfg.emergency_temp)" "$T_FAN"
mut "静默兜底 >= 改成 >" "$L/openfi_fanctl.c" \
	"if (unified_temp >= st->silent_max_temp)" "if (unified_temp > st->silent_max_temp)" "$T_FAN"
mut "切档时不重置迟滞" "$L/openfi_fanctl.c" \
	"curve_hyst_init(&st->hyst, st->hyst.min_delta, st->hyst.interval_ms);" "(void)0;" "$T_FAN"

echo "── 开关解析 ──"
mut "竖线少跳一个字符" "$L/openfi_switch.c" "const char *q = p + ll + 1;" "const char *q = p + ll;" "$T_SW"
mut "静默判定取反" "$L/openfi_switch.c" \
	"return lvl == SW_HIGH ? 1 : 0;" "return lvl == SW_LOW ? 1 : 0;" "$T_SW"

echo "── 曲线 ──"
mut "阶梯查找 <= 改成 <" "$L/openfi_curve.c" "if (x <= c->pt[i].x) {" "if (x < c->pt[i].x) {" "$T_CURVE"
mut "去掉上限夹取" "$L/openfi_curve.c" \
	"	if (v > c->max_y)
		return c->max_y;" "" "$T_CURVE"
mut "去掉下限夹取" "$L/openfi_curve.c" \
	"	if (v < c->min_y)
		return c->min_y;" "" "$T_CURVE"

echo
echo "  变异:抓到 $caught 个,漏掉 $missed 个"
[ "$missed" -eq 0 ] || { echo "  ⚠️ 有盲区,去补测试"; exit 1; }
