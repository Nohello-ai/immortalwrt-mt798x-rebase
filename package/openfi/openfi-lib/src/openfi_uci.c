/*
 * OpenFi 后端公共库 —— UCI 配置读取/写入
 * 纯 C,只依赖 stdio/stdlib/string —— 所以【开发机上能直接跑单元测试】。
 * 详见 openfi_uci.h 里的说明。
 */
#define _GNU_SOURCE
#include "openfi_uci.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UCI_ROOT_DEFAULT "/etc/config"
#define LINE_MAX_ 512

static const char *uci_root = UCI_ROOT_DEFAULT;

void ouci_set_root(const char *root)
{
	uci_root = root ? root : UCI_ROOT_DEFAULT;
}

/* ---- 小工具 ---- */

static char *trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e))
		*e-- = '\0';
	return s;
}

static void unquote(char *s)
{
	size_t n = strlen(s);

	if (n >= 2 && ((s[0] == '\'' && s[n - 1] == '\'') ||
		       (s[0] == '"' && s[n - 1] == '"'))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

/*
 * 解析一条 "option key 'value'" / "list key 'value'"。
 * 匹配则返回 1,并让 key 与 val 指向 line 内部(就地改 \0)。
 * 不匹配返回 0。
 */
static int parse_kv(char *line, const char *type, char **key, char **val)
{
	size_t tl = strlen(type);
	char *p = line;

	if (strncmp(p, type, tl) != 0 || !isspace((unsigned char)p[tl]))
		return 0;
	p = trim(p + tl);
	if (!*p)
		return 0;

	*key = p;
	while (*p && !isspace((unsigned char)*p))
		p++;
	if (!*p) {
		*val = NULL;
		return 1;
	}
	*p++ = '\0';
	*val = trim(p);
	if (*val) {
		unquote(*val);
		/*
		 * 去引号之后【再 trim 一次】。
		 * 严格按 UCI 语义,引号里的空格是值的一部分;但本项目的配置项
		 * 全是枚举/数字/域名这类单 token,带前后空格没有意义,只会让
		 * ouci_get_int/ouci_get_bool 莫名其妙地取到默认值。
		 * 所以这里统一规整 —— 与 shell 里 `uci get` 后立刻用掉的习惯一致。
		 */
		*val = trim(*val);
		/* 注意:显式写成的空值('')保留为空串,不并到 NULL ——
		   "配了但是空的" 和 "压根没配" 语义不同。
		   实际使用时 ouci_get_def/ouci_get_int/ouci_get_bool 把空串当缺省处理。 */
	} else {
		*val = NULL;
	}
	return 1;
}

/*
 * 解析一条 "config <type> ['<name>']"。
 * 把类型写进 type_out,名字写进 name_out(无名则空串)。
 * 匹配则返回 1。
 */
static int parse_config(char *line, char *type_out, size_t tlen,
			char *name_out, size_t nlen)
{
	char *t, *name = NULL, *sp;

	if (strncmp(line, "config", 6) != 0 || !isspace((unsigned char)line[6]))
		return 0;
	t = trim(line + 6);
	if (!*t)
		return 0;

	sp = t;
	while (*sp && !isspace((unsigned char)*sp))
		sp++;
	if (*sp) {
		*sp++ = '\0';
		name = trim(sp);
		if (*name)
			unquote(name);
		else
			name = NULL;
	}
	snprintf(type_out, tlen, "%s", t);
	snprintf(name_out, nlen, "%s", name ? name : "");
	return 1;
}

/* 单遍扫描:找到第 1 个 section 名等于 sec(sec 为空时取第一个匿名段)的段里的 opt */
static char *scan_option(const char *path, const char *sec, const char *opt)
{
	char line[LINE_MAX_], ctype[64], cname[64];
	int in_sec, anonymous;
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return NULL;

	in_sec = 0;
	anonymous = (sec[0] == '\0');

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		char *k, *v;

		if (!*s || *s == '#')
			continue;

		if (parse_config(s, ctype, sizeof(ctype), cname, sizeof(cname))) {
			in_sec = anonymous ? 1 : (cname[0] && !strcmp(cname, sec));
			if (anonymous && in_sec)
				anonymous = 0;
			continue;
		}
		if (!in_sec)
			continue;
		if ((parse_kv(s, "option", &k, &v) || parse_kv(s, "list", &k, &v)) &&
		    k && v && !strcmp(k, opt)) {
			/* ★ 必须在【这里】就复制 —— line 下一轮就被覆盖了 */
			char *out = strdup(v);

			fclose(f);
			return out;
		}
	}
	fclose(f);
	return NULL;
}

/* ---- 对外接口 ---- */

char *ouci_get(const char *pkg, const char *sec, const char *opt)
{
	char path[256];

	if (!pkg || !sec || !opt)
		return NULL;
	snprintf(path, sizeof(path), "%s/%s", uci_root, pkg);
	return scan_option(path, sec, opt);
}

const char *ouci_get_def(const char *pkg, const char *sec, const char *opt, const char *def)
{
	static char buf[256];
	char *v = ouci_get(pkg, sec, opt);

	if (!v)
		return def;
	snprintf(buf, sizeof(buf), "%s", v);
	free(v);
	return buf;
}

long ouci_get_int(const char *pkg, const char *sec, const char *opt, long def)
{
	const char *v = ouci_get_def(pkg, sec, opt, NULL);
	char *end;
	long r;

	if (!v || !*v)
		return def;
	r = strtol(v, &end, 10);
	if (*end)
		return def;
	return r;
}

int ouci_get_bool(const char *pkg, const char *sec, const char *opt, int def)
{
	const char *v = ouci_get_def(pkg, sec, opt, NULL);

	if (!v || !*v)
		return def;
	if (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true") ||
	    !strcmp(v, "yes") || !strcmp(v, "enabled"))
		return 1;
	if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") ||
	    !strcmp(v, "no") || !strcmp(v, "disabled"))
		return 0;
	return def;
}

/* 把值里的单引号转义,再包一层单引号,防止注入(值来自 LuCI 用户输入) */
static void shell_quote(const char *val, char *out, size_t outlen)
{
	size_t j = 0;

	for (size_t i = 0; val[i] && j + 5 < outlen; i++) {
		if (val[i] == '\'') {
			out[j++] = '\'';
			out[j++] = '\\';
			out[j++] = '\'';
			out[j++] = '\'';
		} else {
			out[j++] = val[i];
		}
	}
	out[j] = '\0';
}

int ouci_set(const char *pkg, const char *sec, const char *opt, const char *val)
{
	char cmd[640], esc[320], path[256];
	FILE *f;

	if (!pkg || !sec || !opt || !val)
		return -1;

	/*
	 * ★ 配置文件必须【先存在】。
	 *   新版 uci(25.12 那一代)对不存在的配置名执行 set 会直接报
	 *   "Entry not found" 并返回失败 —— 它不再像老版本那样自动建文件。
	 *   这里先确保文件在,免得"设置保存不上"这种只在真机上才发现的毛病。
	 */
	snprintf(path, sizeof(path), "%s/%s", uci_root, pkg);
	f = fopen(path, "a");		/* 追加模式:不存在就建,存在也不清空 */
	if (!f)
		return -1;
	fclose(f);

	shell_quote(val, esc, sizeof(esc));
	snprintf(cmd, sizeof(cmd), "uci -q set '%s.%s.%s=%s'", pkg, sec, opt, esc);
	if (system(cmd) != 0)
		return -1;
	snprintf(cmd, sizeof(cmd), "uci -q commit '%s'", pkg);
	return system(cmd) == 0 ? 0 : -1;
}

char *ouci_list_options(const char *pkg, const char *sec)
{
	char path[256], line[LINE_MAX_], ctype[64], cname[64], buf[512];
	int in_sec, anonymous, first = 1;
	size_t used = 0;
	FILE *f;

	if (!pkg || !sec)
		return NULL;
	snprintf(path, sizeof(path), "%s/%s", uci_root, pkg);
	f = fopen(path, "r");
	if (!f)
		return NULL;

	buf[0] = '\0';
	in_sec = 0;
	anonymous = (sec[0] == '\0');

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		char *k, *v;
		int n;

		if (!*s || *s == '#')
			continue;
		if (parse_config(s, ctype, sizeof(ctype), cname, sizeof(cname))) {
			in_sec = anonymous ? 1 : (cname[0] && !strcmp(cname, sec));
			if (anonymous && in_sec)
				anonymous = 0;
			continue;
		}
		if (!in_sec)
			continue;
		if (!parse_kv(s, "option", &k, &v) && !parse_kv(s, "list", &k, &v))
			continue;
		if (!k)
			continue;
		n = snprintf(buf + used, sizeof(buf) - used, "%s%s", first ? "" : ",", k);
		if (n < 0 || (size_t)n >= sizeof(buf) - used)
			break;
		used += (size_t)n;
		first = 0;
	}
	fclose(f);
	return strdup(buf);
}
