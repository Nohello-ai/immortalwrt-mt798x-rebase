#!/bin/sh
#
# openfi-modem 端到端测试
#
# 起一个假模组(PTY 模拟真机应答),让 openfi-modem 走完整流程:
#   读配置找串口 → 发一串 AT → 解析 → 写缓存文件 → 输出 JSON
# 全程不需要真模组,也不碰真设备。
#
# 用法: sh e2e.sh <openfi-modem> <fake-modem>
#
# ★ 故意不用 set -e:这个脚本自己统计成败,而且有些用例就是要让程序返回非零。
#
BIN=${1:?用法: sh e2e.sh <openfi-modem> <fake-modem>}
FAKE=${2:?用法: sh e2e.sh <openfi-modem> <fake-modem>}

R=$(mktemp -d)
trap 'rm -rf "$R"; [ -n "$FAKEPID" ] && kill $FAKEPID 2>/dev/null' EXIT

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ✓ $1"; }
bad() { FAIL=$((FAIL+1)); echo "  ✗ $1"; }

json_ok() {
	if command -v python3 >/dev/null 2>&1; then
		printf '%s' "$1" | python3 -c "import json,sys; json.loads(sys.stdin.read())" 2>/dev/null
	elif command -v jsonfilter >/dev/null 2>&1; then
		jsonfilter -s "$1" -e '@' >/dev/null 2>&1
	else
		case "$1" in "{"*"}") return 0 ;; *) return 1 ;; esac
	fi
}
# 从 JSON 里取一个键的值(不带引号)
jget() {
	if command -v python3 >/dev/null 2>&1; then
		# ★ 统一成 JSON 字面量(true/false/null),不要 Python 的 True/False。
		#   否则同一份脚本在开发机上过、在 OpenWrt 上挂(那边没有 python3,
		#   走的是 jsonfilter 分支,输出的是 JSON 字面量)。
		#   这个不一致是 VM 测试抓出来的。
		printf '%s' "$2" | python3 -c "
import json,sys
v = json.load(sys.stdin).get('$1')
if v is True:    print('true')
elif v is False: print('false')
elif v is None:  print('null')
else:            print(v)
" 2>/dev/null
	elif command -v jsonfilter >/dev/null 2>&1; then
		printf '%s' "$2" | jsonfilter -e "@.$1" 2>/dev/null
	else
		printf '%s' "$2" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"
	fi
}

echo "── 启动假模组 ──"
"$FAKE" > "$R/pty" 2>"$R/fake.err" &
FAKEPID=$!
sleep 1
PTY=$(head -1 "$R/pty")
if [ -z "$PTY" ] || [ ! -e "$PTY" ]; then
	bad "假模组没起来(拿不到 pty)"
	echo "  通过 $PASS 项,失败 $FAIL 项"; exit 1
fi
ok "假模组串口: $PTY"

echo "── 造配置(把串口指过去)──"
mkdir -p "$R/etc/config" "$R/var/run"
cat > "$R/etc/config/openfi_modem" <<EOF
config modem 'modem'
	option at_port '$PTY'
	option baud '115200'
	option period '15'
EOF
ok "配置写好了"

export OPENFI_UCI_ROOT="$R/etc/config" OPENFI_SYSFS_ROOT="$R"

echo "── refresh:走完整流程 ──"
OUT=$("$BIN" refresh 2>/dev/null)
if json_ok "$OUT"; then ok "输出是合法 JSON"; else bad "输出不是合法 JSON: $OUT"; fi

echo "── 核对从假模组读到的每一项 ──"
check() {  # check <键> <期望>
	got=$(jget "$1" "$OUT")
	if [ "$got" = "$2" ]; then
		ok "$1 = $got"
	else
		bad "$1 期望 $2 实得 $got"
	fi
}
check ok        true
check imei      012345678901234
check imsi      460001234567890
check iccid     89860112345678901234
check model     RM500U-CN
check firmware  RM500UCNCBR01A03M4G
check sim       1
check operator  'CHINA MOBILE'
check reg       1
check network   NR5G-SA
check band      41
check rssi      21
check rsrp      -81
check rsrq      -10
check sinr      -73
check bars      3
check temp_soc  54
check temp_pa   47

echo "── 缓存文件与温度文件(风扇靠这个温度)──"
if [ -f "$R/var/run/openfi-modem.json" ]; then ok "状态缓存已写入"; else bad "状态缓存没写"; fi
T=$(cat "$R/var/run/openfi-modem-temp" 2>/dev/null)
if [ "$T" = "54" ]; then ok "风扇要的模组温度 = 54"; else bad "温度文件不对: '$T'"; fi

echo "── status:读缓存(零副作用)──"
OUT2=$("$BIN" status 2>/dev/null)
if json_ok "$OUT2"; then ok "status 输出合法 JSON"; else bad "status 输出不合法"; fi
check2=$(jget model "$OUT2")
if [ "$check2" = "RM500U-CN" ]; then ok "status 读到的型号正确"; else bad "status 型号: $check2"; fi

echo "── at:透传一条命令 ──"
A=$("$BIN" at "AT+CGSN" 2>/dev/null)
case "$A" in *012345678901234*) ok "at 透传拿回了 IMEI" ;; *) bad "at 输出: $A" ;; esac

echo
echo "  通过 $PASS 项,失败 $FAIL 项"
[ "$FAIL" -eq 0 ] || exit 1
