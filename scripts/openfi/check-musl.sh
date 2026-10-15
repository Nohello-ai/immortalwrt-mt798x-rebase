#!/bin/sh
#
# 用 musl 把全部源码编译+链接一遍
#
# 【为什么必须有这一步】
#   开发机上装的是 glibc,而固件跑的是 musl。两者对头文件的要求不一样:
#   glibc 会把 <sys/select.h> 之类通过别的头间接带进来,musl 不会。
#   于是同一份代码在本地编得过、到目标机上就是硬错误。
#
#   踩过的实例:重写短信发送链路时用了 select()/fd_set,
#   但没写 #include <sys/select.h> —— 本地 glibc 一路绿灯,
#   CI 编到第 17 步(67 分钟后)才报
#       error: unknown type name 'fd_set'
#   一次白等一个多小时。这个脚本 5 秒就能拦住。
#
# 用法: sh scripts/check-musl.sh
#   没装 musl 工具链时会明确告诉你装哪个包,不算失败。
#
set -u

if ! command -v musl-gcc >/dev/null 2>&1; then
	echo "  跳过:没装 musl 工具链"
	echo "        Debian/Ubuntu:  sudo apt-get install -y musl-tools"
	echo "        Alpine:         apk add musl-dev gcc"
	echo "  ⚠️ 跳过这一步等于放弃"目标环境能不能编过"的检查 ——"
	echo "     glibc 能过、musl 过不了的代码很常见。"
	exit 0
fi

# ── 定位包目录 ──────────────────────────────────────────────
# 支持两种布局,和 run-sanitizers.sh 一样:
#   开发目录: openfi-lib/src
#   固件树:   package/openfi/openfi-lib/src
#
# 【不要再写 `cd "$(dirname "$0")/.."`】
#   踩过:这个脚本本来在 scripts/,后来挪到 scripts/openfi/,
#   那句 `..` 就少退了一级 —— 于是先 cd 到了 scripts/,
#   接着再 cd 相对路径 "scripts/openfi" 直接失败:
#       cd: can't cd to scripts/openfi
#   改成:从脚本自身的【绝对】目录出发向上找,不依赖调用时的 cwd,
#   也不依赖脚本埋在几层目录里。
_sd=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || _sd=$(pwd)
_d="$_sd"
P=.
while [ "$_d" != "/" ]; do
	if [ -d "$_d/openfi-lib/src" ]; then P="$_d"; break; fi
	if [ -d "$_d/package/openfi/openfi-lib/src" ]; then P="$_d/package/openfi"; break; fi
	_d=$(dirname "$_d")
done
if [ ! -d "$P/openfi-lib/src" ]; then
	echo "  找不到包目录(既不是开发目录也不是固件树)"; exit 1
fi
cd "$P" || exit 1

L=openfi-lib/src
M=luci-app-openfi-modem/src
S=luci-app-openfi-sms/src
CF="-O2 -Wall -Wextra -Wpedantic -Wshadow -Werror -std=gnu99"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ok=0; bad=0

echo "  ① 建静态库"
mkdir -p "$WORK/lib"
cp $L/*.c "$WORK/lib/" || { echo "     ❌ 复制源码失败($L)"; exit 1; }
for h in $L/*.h; do [ -f "$h" ] && cp "$h" "$WORK/lib/"; done
(
	cd "$WORK/lib" || exit 1
	for f in *.c; do
		musl-gcc $CF -c "$f" -o "${f%.c}.o" || exit 1
	done
	ar rcs libopenfi.a ./*.o
) || { echo "     ❌ openfi-lib 编译失败"; exit 1; }
echo "     ✅ openfi-lib"

echo "  ② 三个程序:编译 + 链接"

build_one() {
	pkg="$1"; bin="$2"; shift 2
	d="$WORK/$pkg"
	mkdir -p "$d"
	# 有的包没有自己的头文件(fan 就只有 .c),所以 .c 和 .h 分开拷
	cp "$pkg"/src/*.c "$d/" || { echo "     ❌ 复制源码失败($pkg)"; bad=$((bad+1)); return; }
	for h in "$pkg"/src/*.h; do
		[ -f "$h" ] && cp "$h" "$d/"
	done
	if ( cd "$d" && musl-gcc $CF "$@" -I"$WORK/lib" -o "$bin" ./*.c \
			-L"$WORK/lib" -lopenfi ) 2>"$WORK/err"; then
		printf "     ✅ %-14s %s 字节\n" "$bin" "$(stat -c%s "$d/$bin" 2>/dev/null)"
		ok=$((ok+1))
	else
		printf "     ❌ %-14s\n" "$bin"
		head -20 "$WORK/err" | sed 's/^/         /'
		bad=$((bad+1))
	fi
}

build_one luci-app-openfi-fan   openfi-fan   "-I$L"
build_one luci-app-openfi-modem openfi-modem "-I$L" "-I$M"
build_one luci-app-openfi-sms   openfi-sms   "-I$L" "-I$S"

echo
if [ "$bad" -eq 0 ]; then
	echo "  ✅ musl 下编译链接全部通过"
else
	echo "  ❌ 有 $bad 个过不了 —— 这一定会让固件构建失败,先修掉"
	exit 1
fi
