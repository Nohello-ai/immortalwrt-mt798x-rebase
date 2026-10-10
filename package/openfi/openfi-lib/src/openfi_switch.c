#define _GNU_SOURCE
#include "openfi_switch.h"
#include "openfi_sysfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DBG_DEFAULT "/sys/kernel/debug/gpio"

/*
 * debugfs 里那一行的格式(真机抓下来的):
 *  gpio-512 (                    |func                ) in  lo IRQ
 *  gpio-513 (                    |reset               ) in  hi IRQ ACTIVE LOW
 *                              ↑ label 在竖线之间,后面是方向 + 电平
 * 我们要判据:
 *   ① 竖线之间的 label 和请求的完全一致(前后可以有空白)
 *   ② label 之后要先出现 " in "(输入方向)
 *   ③ 然后找 " hi " 还是 " lo "
 */
enum sw_level sw_parse(const char *content, const char *label)
{
	char needle[64];
	const char *p;
	size_t ll;

	if (!content || !label || !*label)
		return SW_UNKNOWN;
	ll = strlen(label);
	if (ll + 2 >= sizeof(needle))
		return SW_UNKNOWN;
	snprintf(needle, sizeof(needle), "|%s", label);

	/* needle 是 "|" + label,总长 ll+1 —— 别忘了那个竖线,否则整个判据全错 */
	for (p = content; (p = strstr(p, needle)); p += ll + 1) {
		const char *q = p + ll + 1;

		/* label 后面必须紧跟空白或竖线(避免 funcX 匹配到 func) */
		if (*q && !isspace((unsigned char)*q) && *q != '|')
			continue;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q == '|')
			q++;
		/* 现在应该到了 ")" 之后的方向/电平区 */
		const char *r = strchr(q, ')');
		if (!r)
			continue;
		r++;
		/* 方向必须是 in */
		const char *in = strstr(r, " in ");
		const char *out = strstr(r, " out ");
		if (!in || (out && out < in))
			continue;
		/* 从 in 之后找电平 */
		const char *hi = strstr(in, " hi ");
		const char *lo = strstr(in, " lo ");
		if (hi && (!lo || hi < lo))
			return SW_HIGH;
		if (lo)
			return SW_LOW;
		return SW_UNKNOWN;
	}
	return SW_UNKNOWN;
}

enum sw_level sw_read_level(const char *dbg_path, const char *label)
{
	enum sw_level r;

	/*
	 * 必须整读:osys_read 是按"读 sysfs 的一个值"设计的(fgets 到 256 字节),
	 * 而 debugfs/gpio 是多行文件,开关那一行未必在前 256 字节里。
	 */
	char full[256];
	FILE *f;
	char buf[8192];
	size_t n;

	osys_path(full, sizeof(full), dbg_path ? dbg_path : DBG_DEFAULT);
	f = fopen(full, "r");
	if (!f)
		return SW_UNKNOWN;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	r = sw_parse(buf, label ? label : "func");
	return r;
}

int sw_is_silent(enum sw_level lvl, int silent_when_hi)
{
	if (lvl == SW_UNKNOWN)
		return -1;
	if (silent_when_hi)
		return lvl == SW_HIGH ? 1 : 0;
	return lvl == SW_LOW ? 1 : 0;
}

const char *sw_level_str(enum sw_level lvl)
{
	switch (lvl) {
	case SW_LOW:  return "low";
	case SW_HIGH: return "high";
	default:      return "unknown";
	}
}
