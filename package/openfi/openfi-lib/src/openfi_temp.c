#define _GNU_SOURCE
#include "openfi_temp.h"
#include "openfi_sysfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void temp_cfg_default(struct temp_cfg *c)
{
	if (!c)
		return;
	memset(c, 0, sizeof(*c));
	snprintf(c->cpu_path, sizeof(c->cpu_path),
		 "/sys/class/thermal/thermal_zone0/temp");
	snprintf(c->wifi_iface, sizeof(c->wifi_iface), "ra0");
	snprintf(c->modem_file, sizeof(c->modem_file),
		 "/var/run/openfi-modem-temp");
	c->wifi_enabled = 1;
}

int temp_parse_millideg(const char *s)
{
	long v;
	char *end;

	if (!s)
		return TEMP_INVALID;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return TEMP_INVALID;
	v = strtol(s, &end, 10);
	if (end == s)
		return TEMP_INVALID;
	/* 合理的摄氏度范围 -40..150 → 毫度 -40000..150000 */
	if (v < -40000 || v > 150000)
		return TEMP_INVALID;
	return (int)(v / 1000);
}

int temp_parse_celsius(const char *s)
{
	long v;
	char *end;

	if (!s)
		return TEMP_INVALID;
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return TEMP_INVALID;
	v = strtol(s, &end, 10);
	if (end == s)
		return TEMP_INVALID;
	if (v < -40 || v > 150)
		return TEMP_INVALID;
	return (int)v;
}

/*
 * 从 iwpriv 输出里找 CurrentTemperature。
 * 真机看到的几种写法(都覆盖):
 *   "ra0      CurrentTemperature: 54"
 *   "CurrentTemperature = 54"
 *   "CurrentTemperature:54"
 * 找不到返回 TEMP_INVALID。
 */
int temp_parse_iwpriv(const char *out)
{
	static const char *key = "CurrentTemperature";
	const char *p;

	if (!out)
		return TEMP_INVALID;
	for (p = out; (p = strstr(p, key)); p += strlen(key)) {
		const char *q = p + strlen(key);

		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != ':' && *q != '=')
			continue;		/* 比如 CurrentTemperatureXyz,跳过 */
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		return temp_parse_celsius(q);
	}
	return TEMP_INVALID;
}

/* 跑一条命令并取输出(内部用;失败返回 NULL) */
static char *run_capture(const char *cmd)
{
	FILE *f = popen(cmd, "r");
	char buf[1024] = "";
	size_t n;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	pclose(f);	/* ★ 必须是 pclose:fclose 会漏掉子进程和退出码 */
	if (n == 0)
		return NULL;
	buf[n] = '\0';
	return strdup(buf);
}

int temp_read(enum temp_src src, const struct temp_cfg *cfg)
{
	char *v, cmd[256];
	int r;

	if (!cfg)
		return TEMP_INVALID;
	switch (src) {
	case TEMP_CPU:
		v = osys_read(cfg->cpu_path);
		if (!v)
			return TEMP_INVALID;
		r = temp_parse_millideg(v);
		free(v);
		return r;

	case TEMP_WIFI:
		if (!cfg->wifi_enabled || !cfg->wifi_iface[0])
			return TEMP_INVALID;
		snprintf(cmd, sizeof(cmd), "iwpriv %s stat 2>/dev/null",
			 cfg->wifi_iface);
		v = run_capture(cmd);
		if (!v)
			return TEMP_INVALID;
		r = temp_parse_iwpriv(v);
		free(v);
		return r;

	case TEMP_MODEM:
		v = osys_read(cfg->modem_file);
		if (!v)
			return TEMP_INVALID;
		r = temp_parse_celsius(v);
		free(v);
		return r;

	default:
		return TEMP_INVALID;
	}
}

int temp_read_all(const struct temp_cfg *cfg, int out3[TEMP_SRC_COUNT], int *unified)
{
	int got = 0, best = TEMP_INVALID;

	for (int i = 0; i < TEMP_SRC_COUNT; i++) {
		int t = temp_read((enum temp_src)i, cfg);

		if (out3)
			out3[i] = t;
		if (t != TEMP_INVALID) {
			got++;
			if (best == TEMP_INVALID || t > best)
				best = t;
		}
	}
	if (unified)
		*unified = best;
	return got;
}
