#!/bin/sh
#
# openfi-fan 端到端冒烟测试
#
# 不用真设备:在临时目录里造一棵【假的设备树】(sysfs + UCI 配置),
# 然后走环境变量把 root 指过去,把守护进程真跑一遍 ——
# 包括 PWM 写入、反极性换算、坐滞、静默档、过热兜底、JSON 输出。
#
# 以前 shell 版只能上机看风扇转不转;现在改一行代码就能在本机全测。
#
# 用法: sh smoke.sh <openfi-fan 可执行文件路径>
#
# ★ 故意【不用 set -e】:
#   这个脚本自己统计通过/失败项,而有些用例【就是要让程序返回非零】
#   (比如"PWM 不存在时应当干净报错")。开着 set -e 会让脚本在做完最后一个
#   用例后静默退出,连汇总行都不打印 —— 表面看像"跑完了",实际少跑了一大段。
#   这个坑在开发机和虚拟机上表现一样,是靠"输出里没有汇总行"才发现的。

BIN=${1:?用法: sh smoke.sh <openfi-fan 路径>}
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN"; exit 1; }

R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT

mkdir -p "$R/sys/class/pwm/pwmchip0/pwm0" "$R/sys/class/pwm/pwmchip0/pwm1" \
         "$R/sys/class/thermal/thermal_zone0" "$R/etc/config" "$R/var/run" \
         "$R/sys/kernel/debug"
# 模拟物理开关。格式照抄真机 cat /sys/kernel/debug/gpio 的输出
set_switch() {
	cat > "$R/sys/kernel/debug/gpio" <<-EOF
	gpiochip0: GPIOs 512-568, parent: platform/11d00000.pinctrl, pinctrl_moore:
	 gpio-512 (                    |func                ) in  $1 IRQ 
	 gpio-513 (                    |reset               ) in  hi IRQ ACTIVE LOW
	EOF
}
set_switch lo
# 真 sysfs 里 period/duty_cycle/enable 本来就在
for ch in 0 1; do
	for a in period duty_cycle enable; do : > "$R/sys/class/pwm/pwmchip0/pwm$ch/$a"; done
done
echo 52000 > "$R/sys/class/thermal/thermal_zone0/temp"
echo 44    > "$R/var/run/openfi-modem-temp"
cat > "$R/etc/config/openfi" <<'EOF'
config fan 'fan'
	option period '5'
	option min_speed '10'
	option temp1 '55'
	option temp2 '58'
	option temp3 '61'
	option temp4 '65'
	option speed1 '30'
	option speed2 '50'
	option speed3 '80'
	option speed4 '100'
	option emergency_temp '85'
	option silent_max_temp '75'
	option wifi_temp '0'
EOF

# JSON 合法性检查:开发机用 python3,OpenWrt 上用 jsonfilter,都没有就粗查
json_ok() {
	if command -v python3 >/dev/null 2>&1; then
		printf '%s' "$1" | python3 -c "import json,sys; json.loads(sys.stdin.read())" 2>/dev/null
	elif command -v jsonfilter >/dev/null 2>&1; then
		jsonfilter -s "$1" -e '@' >/dev/null 2>&1
	else
		case "$1" in "{"*"}") return 0 ;; *) return 1 ;; esac
	fi
}

PASS=0; FAIL=0
export OPENFI_SYSFS_ROOT="$R" OPENFI_UCI_ROOT="$R/etc/config"

duty()   { head -1 "$R/sys/class/pwm/pwmchip0/pwm0/duty_cycle"; }
setmp()  { echo $(($1 * 1000)) > "$R/sys/class/thermal/thermal_zone0/temp"; }

# check <说明> <期望 duty>
check() {
	got=$(duty)
	if [ "$got" = "$2" ]; then
		PASS=$((PASS+1)); echo "  ✓ $1  (duty=$got)"
	else
		FAIL=$((FAIL+1)); echo "  ✗ $1  期望 duty=$2 实得=$got"
	fi
}

echo "── PWM 初始化 ──"
"$BIN" --once >/dev/null 2>&1
for ch in 0 1; do
	p=$(head -1 "$R/sys/class/pwm/pwmchip0/pwm$ch/period")
	e=$(head -1 "$R/sys/class/pwm/pwmchip0/pwm$ch/enable")
	if [ "$p" = "40000" ] && [ "$e" = "1" ]; then
		PASS=$((PASS+1)); echo "  ✓ pwm$ch period=40000 enable=1"
	else
		FAIL=$((FAIL+1)); echo "  ✗ pwm$ch period=$p enable=$e"
	fi
done

echo "── 温度曲线(反极性:duty 越小越快)──"
setmp 30; "$BIN" --once >/dev/null 2>&1; check "30℃ ≤t1 → s1=30% → duty 28000" 28000
setmp 56; "$BIN" --once >/dev/null 2>&1; check "56℃ → s2=50% → duty 20000" 20000
setmp 61; "$BIN" --once >/dev/null 2>&1; check "61℃ ≤t3 → s3=80% → duty 8000"  8000
setmp 70; "$BIN" --once >/dev/null 2>&1; check "70℃ >t4 → s4=100% → duty 0"  0

echo "── 静默档(物理开关拨到 high)──"
# ★ 注意:守护进程是【每轮自己读开关电平】,不依赖任何运行时标志文件。
#   所以这里模拟的是 debugfs 里的电平,不是 /var/run 里的标志 ——
#   这样重启后没人拨过开关也能判对(以前靠标志文件时会判错)。
set_switch hi
setmp 30; "$BIN" --once >/dev/null 2>&1; check "开关 high + 30℃ → 停转 → duty 40000" 40000
setmp 80; "$BIN" --once >/dev/null 2>&1; check "开关 high + 80℃(>75 兜底)→ 强制开" 0
set_switch lo
setmp 30; "$BIN" --once >/dev/null 2>&1; check "拨回 low + 30℃ → 恢复按曲线 → duty 28000" 28000

echo "── 紧急温度 ──"
setmp 90; "$BIN" --once >/dev/null 2>&1; check "90℃(>85)→ 拉满 → duty 0" 0

echo "── 开关读不到时保持现状 ──"
setmp 30; "$BIN" --once >/dev/null 2>&1            # 正常档,转速 30%
rm -f "$R/sys/kernel/debug/gpio"
"$BIN" --once >/dev/null 2>&1
check "debugfs 不可读时不该乱动转速" 28000

echo "── 手动转速 ──"
"$BIN" --speed 60 >/dev/null 2>&1; check "--speed 60 → duty 16000" 16000
"$BIN" --speed 0  >/dev/null 2>&1; check "--speed 0  → duty 40000" 40000
"$BIN" --speed 100 >/dev/null 2>&1; check "--speed 100 → duty 0" 0

echo "── --status 只读且 JSON 合法 ──"
setmp 52; "$BIN" --once >/dev/null 2>&1
before=$(duty)
out=$("$BIN" --status 2>&1)
after=$(duty)
if json_ok "$out" && printf '%s' "$out" | grep -q '"switch"' \
   && printf '%s' "$out" | grep -q '"target"' \
   && printf '%s' "$out" | grep -q '"reason"'; then
	PASS=$((PASS+1)); echo "  ✓ --status 输出合法 JSON(含 switch/target/reason 字段)"
else
	FAIL=$((FAIL+1)); echo "  ✗ --status 输出不是合法 JSON: $out"
fi
if [ "$before" = "$after" ]; then
	PASS=$((PASS+1)); echo "  ✓ --status 零副作用(duty 未变)"
else
	FAIL=$((FAIL+1)); echo "  ✗ --status 改了 duty: $before → $after"
fi

echo "── 异常输入不崩 ──"
bad=$(mktemp -d) || bad=/tmp/openfi-smoke-bad
mkdir -p "$bad/etc/config" "$bad/var/run"
code=0
OPENFI_SYSFS_ROOT="$bad" OPENFI_UCI_ROOT="$bad/etc/config" "$BIN" --once >/dev/null 2>&1 || code=$?
# PWM 写不进去时【就该】报错退出(非零),但不能是被信号打死(>=128 说明崩了)
if [ "$code" -eq 1 ]; then
	PASS=$((PASS+1)); echo "  ✓ PWM 不存在时干净报错(退出码 1,没崩)"
elif [ "$code" -ge 128 ]; then
	FAIL=$((FAIL+1)); echo "  ✗ PWM 不存在时进程异常终止(退出码 $code)"
else
	FAIL=$((FAIL+1)); echo "  ✗ PWM 不存在时期望退出码 1,实得 $code"
fi
rm -rf "$bad"

echo
echo "  通过 $PASS 项,失败 $FAIL 项"
[ "$FAIL" -eq 0 ] || exit 1
