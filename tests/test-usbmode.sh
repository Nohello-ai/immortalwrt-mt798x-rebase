#!/bin/sh
# openfi-usbmode 的纯逻辑回归测试（不碰设备、不切模式）
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export OPENFI_USBMODE_STATE="/tmp/test-usbmode-state.$$"
# 被测脚本在固件的 files/ 覆盖目录里（不属于任何 LuCI 包）
SRC="$HERE/../files/usr/sbin/openfi-usbmode"

pass=0; fail=0
ok()  { printf '    ✅ %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '    ❌ %s\n' "$1"; fail=$((fail+1)); }
eq()  { [ "$2" = "$3" ] && ok "$1" || bad "$1（期望 $3，实际 $2）"; }

# 把脚本当库载入（--library 分支只定义函数，不执行探测）
set -- --library
. "$SRC"

# ── 模式名
eq "模式 5 = NCM"    "$(mode_name 5)" "NCM"
eq "模式 3 = RNDIS"  "$(mode_name 3)" "RNDIS"
eq "模式 0 = RMNET"  "$(mode_name 0)" "RMNET"
eq "未知模式有兜底显示" "$(mode_name 9)" "未知(9)"

# ── 驱动映射（找网卡靠它，不能错）
eq "NCM 用 cdc_ncm"      "$(mode_driver 5)" "cdc_ncm"
eq "RNDIS 用 rndis_host" "$(mode_driver 3)" "rndis_host"
eq "ECM 用 cdc_ether"    "$(mode_driver 1)" "cdc_ether"

# ── proto 映射（写错就上不了网）
eq "NCM 用 dhcp"         "$(mode_proto 5)" "dhcp"
eq "RNDIS 用 dhcp"       "$(mode_proto 3)" "dhcp"

# ── 探测顺序：必须是 NCM → RNDIS，且不含 QMI/RMNET
#    故意不含 MBIM：完整实现移到 mbim-archive 分支了。理由见脚本注释。
#    还会让模组内部网络栈失效（后台进不去），实测收益只有 1.3ms。
eq "探测顺序" "$CHAIN" "5 3"
case "$CHAIN" in
	*0*|*4*) bad "探测链里不该出现 RMNET(0)/QMI(4)" ;;
	*)       ok "探测链里没有 RMNET/QMI（展锐平台不支持）" ;;
esac

# ── 状态文件读写
state_set 5 wwan0 dhcp
eq "state 读 mode"  "$(state_get mode)"  "5"
eq "state 读 dev"   "$(state_get dev)"   "wwan0"
eq "state 读 proto" "$(state_get proto)" "dhcp"
grep -q '^ts=' "$STATE" && ok "state 里有时间戳" || bad "state 缺时间戳"
rm -f "$STATE"
eq "没状态文件时返回空" "$(state_get mode)" ""

# ── 失败记忆（关键安全机制：防止反复卡在坏模式上）
state_set 3 usb0 dhcp
eq "初始没有失败记忆" "$(state_get failed)" ""
state_mark_failed 1
eq "记下失败模式 1" "$(state_get failed)" "1"
state_mark_failed 5
eq "再记失败模式 5" "$(state_get failed)" "1 5"
state_mark_failed 1
eq "重复记不叠加" "$(state_get failed)" "1 5"
# 失败列表要能挡住跳过判断
failed="$(state_get failed)"
case " $failed " in
	*" 1 "*) ok "模式 1 会被跳过（不再重试）" ;;
	*)       bad "模式 1 没被挡住" ;;
esac
case " $failed " in
	*" 3 "*) bad "模式 3(RNDIS)不该在失败列表里" ;;
	*)       ok "RNDIS 不在失败列表（兜底始终可用）" ;;
esac
rm -f "$STATE"

# ── 参数校验
[ "$fail" -eq 0 ]

# ── 进度状态(start/status 这套,界面靠它轮询避免 XHR 超时)──
#    真机上踩过的坑:按钮同步等 auto 跑完(1~2 分钟),
#    而 rpcd 的 fs.exec 约 30 秒就超时 → "Error: XHR request timed out"。
progress_set running "正在探测"
eq "进度:running"     "$(sed -n 's/^status=//p' "$PROGRESS")" "running"
eq "进度:说明"        "$(sed -n 's/^text=//p' "$PROGRESS")" "正在探测"
grep -q '^ts=' "$PROGRESS" && ok "进度里有时间戳" || bad "进度缺时间戳"

progress_set done "完成"
eq "进度:done"        "$(sed -n 's/^status=//p' "$PROGRESS")" "done"

# 进度和状态要能一次请求都拿到(status 动作会拼上 state)
state_set 5 wwan0 dhcp
out="$(progress_show)"
case "$out" in
	*"status=done"*) ok "status 里带进度" ;;
	*) bad "status 里没带进度" ;;
esac
case "$out" in
	*"mode=5"*) ok "status 里带当前模式" ;;
	*) bad "status 里没带当前模式" ;;
esac

# ── AT 口查找:必须能复用 modem 采样结果(真机上就是这里错了)──
#    上一版按固定顺序猜,选中了 ttyUSB3(Modem/PPP 口),
#    AT 发过去没人理 → "当前模式: 未知" + "切换命令被拒"。
grep -q 'openfi-modem.json' "$SRC" && ok "AT 口会复用 modem 的采样结果" \
	|| bad "AT 口没复用 modem 的采样结果"
grep -q 'ttyUSB2' "$SRC" && ok "兜底顺序里有 ttyUSB2" || bad "兜底顺序里没有 ttyUSB2"

printf '\n    %s\n' "usbmode：通过 $pass 项，失败 $fail 项"
