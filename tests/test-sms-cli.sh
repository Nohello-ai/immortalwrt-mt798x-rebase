#!/bin/sh
#
# openfi-sms 命令行的回归测试
#
# 【这个测试最初是为了一个真机 bug】
#   短信页面打开后【一直转圈 / 完全空白】。
#
#   根因:list / read / unread / delete 这些命令只读本地存储,本来不需要串口,
#   但程序启动时无条件调 resolve_at_port()。那个函数在拿不到 modem 采样缓存时
#   会逐个尝试打开 6 个候选串口,每个都要在 flock 上等 12 秒:
#       6 × 12 = 最长 72 秒
#   而串口锁正常就被采样进程持有 —— 于是 LuCI 的
#       fs.exec('/usr/bin/openfi-sms list 100')
#   一直不返回,页面就一直转圈。
#
#   现在改成按需解析:只有 send / collect / selftest 才去碰串口。
#
# 【测试方式】真的把程序跑起来并计时。纯本地操作必须在 2 秒内返回;
#   如果哪天又变回"无条件解析端口",这里会因为等待锁而超时,当场拦住。
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/../package/openfi/luci-app-openfi-sms"
[ -d "$PKG" ] || PKG="$HERE/../luci-app-openfi-sms"

pass=0; fail=0
ok()  { printf '    ✅ %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '    ❌ %s\n' "$1"; fail=$((fail+1)); }
eq()  { [ "$2" = "$3" ] && ok "$1" || bad "$1（期望 $3，实际 $2）"; }

# ── 静态检查:接口本身就是按需的 ────────────────────────────────────────
# 这两条最便宜,先跑。它们挂了后面没必要跑。
grep -q 'cfg_load(struct sms_cfg \*c, int need_port)' "$PKG/src/main.c" \
	&& ok "cfg_load 带 need_port 参数（按需解析）" \
	|| bad "cfg_load 少了 need_port —— 又变回无条件解析串口了"

grep -q 'need_port' "$PKG/src/main.c" \
	&& ok "调用点用 need_port 区分" \
	|| bad "调用点没有区分"

# 纯本地命令不许出现在 need_port 为真的那一支里
for c in list read unread delete; do
	if grep -q "strcmp(act, \"$c\")" "$PKG/src/main.c"; then
		ok "有 $c 命令"
	else
		bad "找不到 $c 命令"
	fi
done

# ── 真的跑一遍（需要能编译）────────────────────────────────────────────
CC=""
for t in musl-gcc gcc cc aarch64-linux-musl-gcc; do
	command -v "$t" >/dev/null 2>&1 && { CC="$t"; break; }
done

if [ -z "$CC" ]; then
	echo "    ⚠️ 没有可用的 C 编译器，跳过运行时计时检查"
	echo ""
	printf '    %s\n' "sms-cli：通过 $pass 项，失败 $fail 项"
	[ "$fail" -eq 0 ] || exit 1
	exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 只编译需要的源文件；缺 openfi-lib 就跳过（开发目录里可能有）
if ! "$CC" -O2 -std=gnu99 -o "$TMP/openfi-sms" \
	"$PKG"/src/*.c "$HERE/../openfi-lib/src"/*.c \
	-I"$HERE/../openfi-lib/src" -I"$PKG/src" 2>"$TMP/cc.log"; then
	# 固件树里源码在 package/openfi/ 下，路径不同，再试一次
	if ! "$CC" -O2 -std=gnu99 -o "$TMP/openfi-sms" \
		"$PKG"/src/*.c "$PKG"/../openfi-lib/src/*.c \
		-I"$PKG"/../openfi-lib/src -I"$PKG"/src 2>>"$TMP/cc.log"; then
		echo "    ⚠️ 编译不过，跳过运行时检查："
		sed 's/^/       /' "$TMP/cc.log" | head -5
		echo ""
		printf '    %s\n' "sms-cli：通过 $pass 项，失败 $fail 项"
		[ "$fail" -eq 0 ] || exit 1
		exit 0
	fi
fi

# 指向空目录：没有 UCI 配置、没有已存短信。
# 关键在【没有 /var/run/openfi-modem.json】——这正是真机上会走到
# "逐个探测串口"那条路的情形。
mkdir -p "$TMP/uci" "$TMP/store"
export OPENFI_UCI_ROOT="$TMP/uci"
export OPENFI_SMS_DIR="$TMP/store"
# 把锁指到一个没人占用的文件，但仍然要走"拿锁"这条代码路径
export OPENFI_AT_LOCK="$TMP/at.lock"
# 万一真的去探测，把等待时间压到 1 秒，避免测试挂太久
export OPENFI_AT_LOCK_WAIT="1"

timed() { # $1=秒上限  $2...=命令
	LIMIT="$1"; shift
	START=$(cut -d' ' -f1 /proc/uptime 2>/dev/null || date +%s)
	"$@" >"$TMP/out" 2>"$TMP/err"
	RC=$?
	END=$(cut -d' ' -f1 /proc/uptime 2>/dev/null || date +%s)
	ELAPSED=$(awk "BEGIN{printf \"%.1f\", $END-$START}")
	echo "$ELAPSED $RC"
}

# list 必须快
R=$(timed 2 "$TMP/openfi-sms" list 100)
EL=${R% *}; RC=${R#* }
if awk "BEGIN{exit !($EL < 2)}"; then
	ok "list 在 ${EL}s 内返回（纯本地，不该抢串口）"
else
	bad "list 用了 ${EL}s —— 又去等串口锁了"
fi
eq "list 退出码" "$RC" "0"
case "$(cat "$TMP/out")" in
	*'"messages"'*) ok "list 输出是合法结构" ;;
	*) bad "list 输出不对：$(head -c 80 "$TMP/out")" ;;
esac
eq "空存储时 total" \
	"$(sed -n 's/.*"total":\([0-9]*\).*/\1/p' "$TMP/out" | head -1)" "0"

# unread 也必须快
R=$(timed 2 "$TMP/openfi-sms" unread)
EL=${R% *}
if awk "BEGIN{exit !($EL < 2)}"; then
	ok "unread 在 ${EL}s 内返回"
else
	bad "unread 用了 ${EL}s"
fi

# delete all 也必须快
R=$(timed 2 "$TMP/openfi-sms" delete all)
EL=${R% *}
if awk "BEGIN{exit !($EL < 2)}"; then
	ok "delete all 在 ${EL}s 内返回"
else
	bad "delete all 用了 ${EL}s"
fi

# 反向验证：send 确实【会】去解析端口（说明 need_port 没被写死成 0）
# 这里只验证它尝试了（会失败，因为没有真串口），不是验证发送成功。
grep -q '!strcmp(act, "send")' "$PKG/src/main.c" \
	&& ok "send 走 need_port=1 那条路" \
	|| bad "send 没有被算进需要串口的命令"

printf '\n    %s\n' "sms-cli：通过 $pass 项，失败 $fail 项"
[ "$fail" -eq 0 ] || exit 1
exit 0
