#!/bin/sh
# shellcheck disable=SC2002  # 这里必须用 `cat 文件 | ssh "cat > 目标"` 的写法:
#                           # OpenWrt 精简系统里没有 sftp-server,scp 用不了。
#
# 在 OpenWrt 虚拟机里验证(不是模拟 —— 真 OpenWrt / aarch64 / musl)
#
# 做三件事:
#   ① 交叉编译 aarch64 静态二进制
#      (代码零外部依赖,所以这一步不需要任何目标机的库)
#   ② 传进 VM 跑完整冒烟
#   ③ 在 VM 里验证 UCI 默认值脚本、启动脚本、开关脚本
#
# 用法:
#   sh scripts/test-in-vm.sh [VM_IP] [密码]
# 默认 192.168.54.129 / 123456
#
# 为什么值得做:本机测试能覆盖"逻辑",但覆盖不了
#   · glibc 编出来的东西在 musl 上能不能跑
#   · aarch64 上有没对齐/32/64 位假设的问题
#   · busybox 的 ash 跟 bash 的差异、少了哪些命令
# 这些只有丢进真 OpenWrt 里才会暴露。
#
set -e

VM=${1:-192.168.54.129}
PW=${2:-123456}
HERE=$(cd "$(dirname "$0")/.." && pwd)

SSH="sshpass -p $PW ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o LogLevel=ERROR root@$VM"
L="$HERE/openfi-lib/src"
FAN="$HERE/luci-app-openfi-fan"
BIN=/tmp/openfi-fan-arm64

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ✓ $1"; }
bad()  { fail=$((fail+1)); echo "  ✗ $1"; }

echo "═══ ① 交叉编译 aarch64 静态二进制 ═══"
aarch64-linux-gnu-gcc -O2 -static -std=gnu99 -I"$L" -o "$BIN" \
	"$FAN/src/openfi-fan.c" \
	"$L/openfi_fanctl.c" "$L/openfi_curve.c" "$L/openfi_temp.c" \
	"$L/openfi_sysfs.c" "$L/openfi_uci.c" "$L/openfi_json.c" \
	"$L/openfi_log.c" "$L/openfi_switch.c"
sz=$(wc -c < "$BIN")
echo "  二进制 $sz 字节"
if aarch64-linux-gnu-readelf -d "$BIN" 2>/dev/null | grep -q NEEDED; then
	bad "还有动态依赖,VM 里可能跑不起来"
else
	ok "完全静态(0 个动态依赖)"
fi

echo
echo "═══ ② 传进 VM 并跑冒烟 ═══"
# OpenWrt 没有 sftp-server,scp 用不了,只能管道传
cat "$BIN" | $SSH "cat > /usr/sbin/openfi-fan && chmod 755 /usr/sbin/openfi-fan"
cat "$FAN/tests/smoke.sh" | $SSH "cat > /tmp/smoke.sh"
echo "  ── VM 上的冒烟结果 ──"
# ★ 注意:不能写 `if cmd | sed ...; then` —— 那判断的是 sed 的退出码,不是 cmd 的。
#   把结果先落到文件,再判状态。
# ★ 注意 set -e:失败的命令直接放一行会让脚本立刻退出,
#   必须放在 if 条件里(条件里的命令不受 set -e 影响)。
if $SSH "sh /tmp/smoke.sh /usr/sbin/openfi-fan" >/tmp/vm-smoke.log 2>&1; then
	sed 's/^/    /' /tmp/vm-smoke.log
	ok "VM 冒烟通过"
else
	sed 's/^/    /' /tmp/vm-smoke.log
	bad "VM 冒烟失败"
fi

echo
echo "═══ ②b 模组包:交叉编译 + 在 VM 里用假模组跑端到端 ═══"
MD="$HERE/luci-app-openfi-modem"
aarch64-linux-gnu-gcc -O2 -static -std=gnu99 -I"$L" -I"$MD/src" -o /tmp/openfi-modem-arm64 \
	"$MD/src/main.c" "$MD/src/openfi_modem.c" \
	"$L/openfi_at.c" "$L/openfi_at_io.c" "$L/openfi_json.c" \
	"$L/openfi_log.c" "$L/openfi_sysfs.c" "$L/openfi_uci.c"
aarch64-linux-gnu-gcc -O2 -static -std=gnu99 -o /tmp/fake-modem-arm64 \
	"$MD/tests/fake-modem.c" -lutil
echo "  二进制: modemp $(wc -c < /tmp/openfi-modem-arm64) 字节 / 假模组 $(wc -c < /tmp/fake-modem-arm64) 字节"
cat /tmp/openfi-modem-arm64 | $SSH "cat > /usr/sbin/openfi-modem && chmod 755 /usr/sbin/openfi-modem"
cat /tmp/fake-modem-arm64  | $SSH "cat > /tmp/fake-modem && chmod 755 /tmp/fake-modem"
cat "$MD/tests/e2e.sh"     | $SSH "cat > /tmp/e2e.sh"
if $SSH "sh /tmp/e2e.sh /usr/sbin/openfi-modem /tmp/fake-modem" >/tmp/vm-modem.log 2>&1; then
	sed 's/^/    /' /tmp/vm-modem.log
	ok "VM 上模组端到端通过(假模组 PTY)"
else
	sed 's/^/    /' /tmp/vm-modem.log
	bad "VM 上模组端到端失败"
fi

echo
echo "═══ ③ VM 上验证配置脚本(UCI 默认值)═══"
cat "$FAN/root/etc/uci-defaults/40_luci-app-openfi-fan" | $SSH "cat > /tmp/ud.sh"
# 在 VM 上真跑一遍:它会写 /etc/config/openfi_fan
if $SSH "
	rm -f /etc/config/openfi_fan
	sh /tmp/ud.sh
	echo '  生成后的配置:'
	uci show openfi 2>&1 | sed 's/^/    /'
	# 校验关键项都写进去了
	for k in fan.mode fan.period fan.temp1 fan.speed4 fan.emergency_temp fan.silent_when_hi; do
		uci -q get openfi.\$k >/dev/null || { echo \"  缺少 \$k\"; exit 1; }
	done
	# 再跑一次不能报错(幂等)
	sh /tmp/ud.sh && echo '  重复执行 OK(幂等)'
" 2>&1 | sed 's/^/  /'; then
	ok "uci-defaults 在 VM 上生成配置成功"
else
	bad "uci-defaults 有问题"
fi

echo
echo "═══ ④ VM 上验证启动脚本语法(busybox ash)═══"
$SSH "
	for f in /tmp/initd.sh /tmp/rcbutton.sh; do rm -f \$f; done
	cat > /tmp/initd.sh <<'X'
$(cat "$FAN/root/etc/init.d/openfi-fan")
X
	cat > /tmp/rcbutton.sh <<'X'
$(cat "$FAN/root/etc/rc.button/config")
X
	sh -n /tmp/initd.sh && echo '  ✓ init.d 脚本语法 OK'
	sh -n /tmp/rcbutton.sh && echo '  ✓ rc.button 脚本语法 OK'
" 2>&1 | sed 's/^/  /' || bad "脚本语法有问题"

echo
echo "  通过 $pass 项,失败 $fail 项"
[ "$fail" -eq 0 ] || exit 1
