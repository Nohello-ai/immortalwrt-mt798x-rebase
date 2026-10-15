#!/bin/sh
#
# 跨包一致性检查
#
# ── 为什么需要这个脚本 ──────────────────────────────────────────────────
#   下面这类 bug 我们已经踩过【三次】,每次都是"改了一处、漏了另一处",
#   而且都能过编译、过单元测试,直到上机点开页面才炸:
#
#     ① 把风扇的配置名从 openfi 改成 openfi_fan,
#        改了 C 代码、改了 ACL、改了 uci.load —— 漏了同一文件里的
#        form.Map('openfi', ...)。结果页面报:
#            RPC call to uci/get failed with ubus code 4: 未找到资源
#
#     ② 同一个改名,漏了 modem 那个包的 ACL(里面还写着 "openfi"),
#        而 /etc/config/openfi 已经不存在 —— LuCI 登录时会把所有 ACL 里的
#        uci 配置都预加载,于是【任何页面】都会去请求它,照样报错。
#
#     ③ 短信页面一直转圈 —— 这个不是配置名,是"命令该不该碰串口"的逻辑。
#
#   共同点:名字/路径这类【跨文件引用】没有单一事实来源,
#   全靠人记着改全。所以这里做一遍机器核对。
#
# ── 检查什么 ────────────────────────────────────────────────────────────
#   对每个包:
#     1. JS 里 uci.load('X') / form.Map('X', ...) 用到的配置名
#     2. ACL 里声明的 uci 权限
#     3. C 代码里 ouci_*("X" 用到的配置名
#     4. uci-defaults 里实际创建的 /etc/config/X
#     → 四者必须指向同一个配置
#
#   5. ACL 是否允许 JS 实际调用到的可执行文件/命令
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

# 定位包目录:支持开发目录和固件树两种布局
_d="$HERE"
P=.
while [ "$_d" != "/" ]; do
	if [ -d "$_d/package/openfi/openfi-lib" ]; then P="$_d/package/openfi"; break; fi
	if [ -d "$_d/luci-app-openfi-fan" ] && [ -d "$_d/openfi-lib" ]; then P="$_d"; break; fi
	_d=$(dirname "$_d")
done
if [ ! -d "$P/openfi-lib" ]; then
	echo "  ✗ 找不到 openfi 包目录（既不是开发目录也不是固件树）"
	exit 1
fi

pass=0; fail=0
ok()  { printf '    ✅ %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '    ❌ %s\n' "$1"; fail=$((fail+1)); }

# 从 JS 里抽 uci 配置名(uci.load('X') 和 form.Map('X', ...))
js_ucis() {
	sed -n "s/.*uci\.load('\([a-z_]*\)').*/\1/p;s/.*form\.Map('\([a-z_]*\)'.*/\1/p" "$1" 2>/dev/null | sort -u
}
# 从 ACL 里抽 uci 权限
acl_ucis() {
	python3 -c "
import json,sys
d=json.load(open(sys.argv[1],encoding='utf-8'))
k=list(d.keys())[0]
out=set()
for s in ('read','write'):
    out.update(d[k][s].get('uci',[]))
print('\n'.join(sorted(out)))
" "$1" 2>/dev/null
}
# 从 uci-defaults 里抽实际创建的配置文件
defcfg_uics() {
	grep -ohE '/etc/config/[a-z_]+' "$1" 2>/dev/null | sed 's|/etc/config/||' | sort -u
}
# 从 C 代码里抽包名
c_ucis() {
	grep -ohE 'ouci_[a-z_]+\("[a-z_]+"' "$1" 2>/dev/null | grep -oE '"[a-z_]+"' | tr -d '"' | sort -u
}

echo "  ── ① 每个包的 uci 配置名是否一致 ──"
for pkg in fan modem sms; do
	DIR="$P/luci-app-openfi-$pkg"
	[ -d "$DIR" ] || continue

	JS="$(ls "$DIR"/htdocs/luci-static/resources/view/openfi/*.js 2>/dev/null | head -1)"
	ACL="$(ls "$DIR"/root/usr/share/rpcd/acl.d/*.json 2>/dev/null | head -1)"
	DEF="$(ls "$DIR"/root/etc/uci-defaults/* 2>/dev/null | head -1)"
	SRC="$(ls "$DIR"/src/*.c 2>/dev/null)"

	j="$( [ -n "$JS" ]  && js_ucis "$JS"       | tr '\n' ' ' )"
	a="$( [ -n "$ACL" ] && acl_ucis "$ACL"     | tr '\n' ' ' )"
	d="$( [ -n "$DEF" ] && defcfg_uics "$DEF"  | tr '\n' ' ' )"
	c="$( [ -n "$SRC" ] && cat $SRC | grep -ohE 'ouci_[a-z_]+\("[a-z_]+"' | grep -oE '"[a-z_]+"' | tr -d '"' | sort -u | tr '\n' ' ' )"

	printf '      %-6s JS=[%s] ACL=[%s] uci-defaults=[%s] C=[%s]\n' "$pkg" "$j" "$a" "$d" "$c"

	# 期望的配置名
	EXP="openfi_$pkg"

	# ACL 里不许出现裸 openfi(那个配置文件不存在)
	if echo " $a " | grep -qE ' openfi '; then
		bad "$pkg: ACL 里还写着裸 \"openfi\" —— /etc/config/openfi 不存在,LuCI 会报 code 4"
	else
		ok "$pkg: ACL 无裸 openfi"
	fi
	# JS 里不许出现裸 openfi
	if echo " $j " | grep -qE ' openfi '; then
		bad "$pkg: JS 里还引用裸 \"openfi\"(uci.load / form.Map)"
	else
		ok "$pkg: JS 无裸 openfi"
	fi
	# ACL 必须包含期望的配置名(如果这个包的页面确实用 uci)
	if [ -n "$j" ] && [ -n "$a" ]; then
		if echo " $a " | grep -q "$EXP"; then
			ok "$pkg: ACL 覆盖 JS 用到的 $EXP"
		else
			bad "$pkg: JS 用了 uci 但 ACL 没给 $EXP"
		fi
	fi
	# uci-defaults 建的配置名要和 C 代码一致
	if [ -n "$d" ] && [ -n "$c" ]; then
		for x in $c; do
			echo " $d " | grep -q " $x " \
				&& ok "$pkg: C 用 $x 且 uci-defaults 会建它" \
				|| bad "$pkg: C 代码用 $x,但 uci-defaults 没建这个配置"
		done
	fi
done

echo "  ── ② ACL 是否放行 JS 实际调用到的命令 ──"
for pkg in fan modem sms; do
	DIR="$P/luci-app-openfi-$pkg"
	JS="$(ls "$DIR"/htdocs/luci-static/resources/view/openfi/*.js 2>/dev/null | head -1)"
	ACL="$(ls "$DIR"/root/usr/share/rpcd/acl.d/*.json 2>/dev/null | head -1)"
	[ -n "$JS" ] && [ -n "$ACL" ] || continue

	# JS 里 fs.exec 的可执行文件路径
	for bin in $(grep -ohE "fs\.exec\('[^']+'" "$JS" 2>/dev/null | sed "s/fs.exec('//;s/'//" | sort -u); do
		# ACL 里有没有这个路径(允许后面带参数模式)
		if python3 -c "
import json,sys
d=json.load(open(sys.argv[1],encoding='utf-8'))
k=list(d.keys())[0]
keys=set(d[k]['read'].get('file',{})) | set(d[k]['write'].get('file',{}))
bin=sys.argv[2]
ok = bin in keys or any(x==bin or x.startswith(bin+' ') for x in keys)
sys.exit(0 if ok else 1)
" "$ACL" "$bin" 2>/dev/null; then
			ok "$pkg: ACL 放行 $bin"
		else
			bad "$pkg: JS 调用 $bin,但 ACL 里没有它 —— 页面上会 Permission denied"
		fi
	done

	# JS 里 fs.exec('/etc/init.d/xxx', [...]) 这种
	for svc in $(grep -ohE "fs\.exec\('/etc/init\.d/[^']+'" "$JS" 2>/dev/null | sed "s|fs.exec('||;s|'||" | sort -u); do
		if grep -q "$svc" "$ACL" 2>/dev/null; then
			ok "$pkg: ACL 放行 $svc"
		else
			bad "$pkg: JS 调用 $svc,但 ACL 里没有"
		fi
	done
done

echo "  ── ③ 菜单 → 视图 → 文件 是否对得上 ──"
for pkg in fan modem sms; do
	DIR="$P/luci-app-openfi-$pkg"
	MENU="$(ls "$DIR"/root/usr/share/luci/menu.d/*.json 2>/dev/null | head -1)"
	[ -n "$MENU" ] || continue

	# 菜单里的 "path": "openfi/xxx"
	for vp in $(grep -oE '"path"[[:space:]]*:[[:space:]]*"[^"]+"' "$MENU" 2>/dev/null | sed 's/.*"path"[^"]*"//;s/"//'); do
		F="$DIR/htdocs/luci-static/resources/view/$vp.js"
		if [ -f "$F" ]; then
			ok "$pkg: 菜单 $vp → 视图文件存在"
		else
			bad "$pkg: 菜单指向 $vp,但没有 $F"
		fi
	done

	# 菜单的 depends.acl 必须和 ACL 文件的顶层 key 一致
	# ★ 【不要用 grep 匹配 "acl": [...]】—— JSON 里可能是多行:
	#       "depends": {
	#           "acl": [
	#               "luci-app-openfi-sms"
	#           ]
	#       }
	#   grep 是逐行匹配的,匹配不到跨行的方括号。
	#   第一版就是这么写的,sms 那项静默漏过去了(17 项里少一项没人发现)。
	#   用 python 解析 JSON 才可靠。
	need=$(python3 -c "
import json,sys
try:
    d=json.load(open(sys.argv[1],encoding='utf-8'))
except Exception:
    sys.exit(0)
for v in d.values():
    for a in v.get('depends',{}).get('acl',[]):
        print(a); break
    break
" "$MENU" 2>/dev/null)
	ACL="$(ls "$DIR"/root/usr/share/rpcd/acl.d/*.json 2>/dev/null | head -1)"
	if [ -n "$need" ] && [ -n "$ACL" ]; then
		have=$(python3 -c "
import json,sys
print(list(json.load(open(sys.argv[1],encoding='utf-8')).keys())[0])
" "$ACL" 2>/dev/null)
		if [ "$need" = "$have" ]; then
			ok "$pkg: 菜单 depends.acl=$need ↔ ACL key 一致"
		else
			bad "$pkg: 菜单要 ACL \"$need\",但 ACL 文件的 key 是 \"$have\""
		fi
	fi
done

echo "  ── ④ 按钮里的 fs.exec 是否都有 .catch ──"
# 【为什么单列这一项】
#   LuCI 的 ui.createHandlerFn 是"先禁用按钮,等返回的 promise 落定再恢复"。
#   如果处理函数返回的 promise 被 reject 而【没有人 catch】,就会出现未处理的
#   rejection —— 恢复那步跑不到,按钮【永久灰掉】,只能刷新页面才能再点。
#
#   真机上短信页面就是这么卡的:查看/删除/全部删除/发送四个按钮
#   当时全都没有 catch。后来一查,风扇的"手动调速"和模组的"硬复位"也有。
#
#   判定方式:从 fs.exec 那一行起做括号配平找到整条语句,
#   再看这条语句里有没有 .catch。
#   ★ 不能只看"接下来 5 行" —— 多行块会漏判(第一版就是这么漏掉两处的)。
for pkg in fan modem sms; do
	JS="$(ls "$P/luci-app-openfi-$pkg"/htdocs/luci-static/resources/view/openfi/*.js 2>/dev/null | head -1)"
	[ -n "$JS" ] || continue

	bad_list=$(python3 - "$JS" <<'PYEOF'
import sys
path = sys.argv[1]
lines = open(path, encoding='utf-8').read().split('\n')
bad = []
for i, l in enumerate(lines):
    if 'fs.exec' not in l or l.strip().startswith(('*', '//')):
        continue
    depth = 0
    end = i
    for j in range(i, min(i + 60, len(lines))):
        depth += lines[j].count('(') - lines[j].count(')')
        end = j
        if depth <= 0 and j > i:
            break
    if '.catch' not in '\n'.join(lines[i:end + 1]):
        bad.append(str(i + 1))
print(' '.join(bad))
PYEOF
)

	if [ -z "$bad_list" ]; then
		ok "$pkg: 所有 fs.exec 都有 .catch"
	else
		bad "$pkg: 第 $bad_list 行的 fs.exec 没有 .catch —— 按钮会永久灰掉"
	fi
done

printf '\n    %s\n' "consistency：通过 $pass 项，失败 $fail 项"
[ "$fail" -eq 0 ] || exit 1
exit 0
