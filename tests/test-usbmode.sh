#!/bin/sh
# openfi-usbmode 的纯逻辑回归测试（不碰设备、不切模式）
#
# 【这个脚本现在只管两件事】
#   show         看当前模式
#   set {1|3|5}  切模式
#
# 曾经有过一整套 auto 自动探测 + start/status 进度轮询。真机上是灾难：
#   · auto 要逐个模式试，每次都切模式 → 必然断网
#   · 跑完还被反复拉起，每分钟切一次 → 网一直被它搞断
# 整个拿掉之后，测试里那些函数(state_mark_failed / progress_set / ...)
# 也都不存在了 —— 这里同步删掉，不然构建会在第 5 步挂掉。
#
# 【教训】精简脚本时必须同步改测试。
#   上一次就是删了函数没删测试，白跑一次 15 分钟的构建。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export OPENFI_USBMODE_STATE="/tmp/test-usbmode-state.$$"
# 被测脚本在固件的 files/ 覆盖目录里（不属于任何 LuCI 包）
SRC="$HERE/../files/usr/sbin/openfi-usbmode"

pass=0; fail=0
ok()  { printf '    ✅ %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '    ❌ %s\n' "$1"; fail=$((fail+1)); }
eq()  { [ "$2" = "$3" ] && ok "$1" || bad "$1（期望 $3，实际 $2）"; }

# 把脚本当库载入（--library 分支只定义函数，不执行任何动作）
set -- --library
. "$SRC"
STATE="${OPENFI_USBMODE_STATE}"

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

# ── 探测顺序
#    故意不含 MBIM：会让模组内部网络栈失效（后台进不去），实测收益只有 1.3ms。
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

# ── AT 收发必须复用 openfi-modem
#    真机上自己用 stty + printf + cat 那套读不到回应：
#    stty 失败会兜底成 true，cat 就阻塞到超时，什么都读不到。
#    表现是"当前模式: 未知"+"切换命令被拒"。
grep -q '/usr/sbin/openfi-modem' "$SRC" && ok "AT 收发复用 openfi-modem" \
	|| bad "AT 收发没有复用 openfi-modem"
grep -q '^at_send()' "$SRC" && ok "有 at_send 封装" || bad "没有 at_send 封装"

# ── 切换后必须让模组重新枚举
#    只发 AT+QCFG="usbnet",N 是【不生效】的 ——
#    真机上验证过：发完驱动照旧是 rndis_host、MAC 也没变。
#    必须再让模组重新枚举一次，新配置才应用。
grep -q 'AT+CFUN=1,1' "$SRC" && ok "切换后重启模组让它重新枚举" \
	|| bad "切换后没触发重新枚举（新配置不会生效）"

# ── 端口解析必须【全部交给 openfi-modem】,脚本自己不许猜
#
#    真机上就是这么错的:脚本按固定顺序猜端口,选中了 ttyUSB3
#    (那是 Modem/PPP 口,不是 AT 口),命令发过去没人理,
#    表现是"当前模式: 未知" + "切换命令被拒"。
#
#    现在 at_send 直接调 openfi-modem at —— 它内部有自己的
#    find_at_port(会真的探哪个口答 AT),比在这里猜靠谱得多。
#    所以脚本里不该再出现任何 /dev/ttyUSB* 的猜测。
if grep -q '/dev/ttyUSB' "$SRC"; then
	bad "脚本还在自己猜端口(应该交给 openfi-modem)"
else
	ok "端口解析全交给 openfi-modem(脚本不猜)"
fi
grep -q '^at_send()' "$SRC" && ok "at_send 是唯一入口" || bad "缺少 at_send 入口"

# ── 命令接口:只留 show / set,不能再有 auto
grep -q '^show)' "$SRC" && ok "有 show 动作" || bad "没有 show 动作"
grep -q '^set)'  "$SRC" && ok "有 set 动作"  || bad "没有 set 动作"
grep -q '^auto)' "$SRC" && bad "还留着 auto 动作（会把网反复搞断）" \
	|| ok "没有 auto 动作（不会自己切模式）"
grep -q '^start)' "$SRC" && bad "还留着 start 动作" || ok "没有 start 动作（界面改成直接 set）"

# ── init.d:开机不能自动探测
INIT="$HERE/../files/etc/init.d/openfi-usbmode"
if [ -f "$INIT" ]; then
	grep -q 'openfi-usbmode auto' "$INIT" && bad "init.d 还在开机自动探测" \
		|| ok "init.d 不在开机自动探测"
else
	bad "找不到 init.d 脚本"
fi

printf '\n    %s\n' "usbmode：通过 $pass 项，失败 $fail 项"

# ★ 必须给退出码。
#   工作流里是 `sh tests/test-usbmode.sh || fail=1` ——
#   不给退出码的话测试挂了构建也照跑，等于白测。
[ "$fail" -eq 0 ] || exit 1
exit 0
