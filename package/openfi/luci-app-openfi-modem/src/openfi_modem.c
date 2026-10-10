#define _GNU_SOURCE
#include "openfi_modem.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void md_state_init(struct md_state *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->sim = MD_SIM_UNKNOWN;
	s->reg = MD_REG_UNKNOWN;
	s->sim_slot = 1;
	s->sim_slots = 1;
	s->rssi = 99;
	s->rsrp = s->rsrq = s->sinr = 0;
	s->temp_soc = s->temp_pa = -1000;
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

	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
		       (s[0] == '\'' && s[n - 1] == '\''))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

/* 取前缀后冒号后面的内容(去空白)。没有冒号则取整行 */
static const char *after_colon(const char *line)
{
	const char *p = line ? strchr(line, ':') : NULL;

	if (!p)
		return line ? line : "";
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* 按逗号切分(就地改),返回字段数。字段内去空白与引号 */
static int split_csv(char *s, char *out[], int max)
{
	int n = 0;
	char *p = s;

	while (p && n < max) {
		char *comma = strchr(p, ',');

		if (comma)
			*comma = '\0';
		out[n] = trim(p);
		unquote(out[n]);
		n++;
		if (!comma)
			break;
		p = comma + 1;
	}
	return n;
}

/* 把字段转成整数;空/非数字 → def */
static long field_long(const char *f, long def)
{
	char *end;
	long v;

	if (!f || !*f || !strcmp(f, "-"))
		return def;
	v = strtol(f, &end, 10);
	if (*end)
		return def;
	return v;
}

const char *md_find_line(const char *resp, const char *prefix)
{
	static char buf[512];
	const char *p;
	size_t pl;

	if (!resp || !prefix)
		return NULL;
	pl = strlen(prefix);
	for (p = resp; *p; ) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[512];

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		char *s = trim(line);

		if (strncmp(s, prefix, pl) == 0) {
			snprintf(buf, sizeof(buf), "%s", s);
			return buf;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return NULL;
}

/* ---- +CSQ ---- */

int md_parse_csq(const char *line, long *rssi)
{
	char tmp[64], *f[4];
	int n;

	if (!line || !rssi)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	n = split_csv(tmp, f, 4);
	if (n < 1)
		return -1;
	long v = field_long(f[0], -1);

	if (v < 0 || v > 31) {
		if (v != 99)
			return -1;
	}
	*rssi = v;
	return 0;
}

/*
 * 3GPP 27.007 的 CSQ → 格数映射。
 * 99 表示未知(视为无信号)。
 *   0..9   → 0 格(几乎没有)
 *   10..14 → 1
 *   15..19 → 2
 *   20..24 → 3
 *   25..29 → 4
 *   30..31 → 5
 */
int md_rssi_to_bars(long rssi)
{
	if (rssi < 0 || rssi == 99)
		return 0;
	if (rssi >= 30) return 5;
	if (rssi >= 25) return 4;
	if (rssi >= 20) return 3;
	if (rssi >= 15) return 2;
	if (rssi >= 10) return 1;
	return 0;
}

/* ---- +CPIN ---- */

enum md_sim md_parse_cpin(const char *line)
{
	if (!line)
		return MD_SIM_UNKNOWN;
	if (strstr(line, "SIM not inserted") || strstr(line, "SIM failure"))
		return MD_SIM_ABSENT;
	/* 没插卡通常回 +CME ERROR: 10(SIM not inserted)*/
	if (strstr(line, "+CME ERROR: 10") || strstr(line, "CME ERROR: 10"))
		return MD_SIM_ABSENT;
	if (strstr(line, "+CME ERROR: 13") || strstr(line, "CME ERROR: 13"))
		return MD_SIM_ERROR;		/* SIM failure */
	if (strstr(line, "+CME ERROR: 14"))
		return MD_SIM_PIN;		/* SIM busy → 当未就绪 */
	if (strstr(line, "READY"))
		return MD_SIM_READY;
	if (strstr(line, "SIM PIN"))
		return MD_SIM_PIN;
	if (strstr(line, "SIM PUK"))
		return MD_SIM_PUK;
	if (strstr(line, "ERROR"))
		return MD_SIM_ERROR;
	return MD_SIM_UNKNOWN;
}

/* ---- +COPS ---- */

int md_parse_cops(const char *line, char *oper, size_t n, char *act, size_t m)
{
	char tmp[256], *f[8];
	int cnt;

	if (!line || !oper)
		return -1;
	oper[0] = '\0';
	if (act && m)
		act[0] = '\0';

	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	cnt = split_csv(tmp, f, 8);
	if (cnt < 3)
		return -1;
	snprintf(oper, n, "%s", f[2]);		/* 形如 CHINA MOBILE;未注网时是空的 */
	for (int i = 3; i < cnt; i++) {
		if (f[i][0] && isdigit((unsigned char)f[i][0])) {
			if (act && m)
				snprintf(act, m, "%s", f[i]);	/* 制式编号 */
			break;
		}
	}
	return oper[0] ? 0 : -1;
}

/* ---- +CREG / +CEREG ---- */

enum md_reg md_parse_creg(const char *line)
{
	char tmp[128], *f[6];
	int n;

	if (!line)
		return MD_REG_UNKNOWN;
	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	n = split_csv(tmp, f, 6);
	if (n < 1)
		return MD_REG_UNKNOWN;
	/* 两字段是 "0,1";单字段是 "1"(有些固件省略 n)*/
	const char *stat = (n >= 2) ? f[1] : f[0];
	long v = field_long(stat, -1);

	switch (v) {
	case 0: return MD_REG_NONE;
	case 1: return MD_REG_HOME;
	case 2: return MD_REG_SEARCHING;
	case 3: return MD_REG_DENIED;
	case 4: return MD_REG_UNKNOWN3;
	case 5: return MD_REG_ROAMING;
	default: return MD_REG_UNKNOWN;
	}
}

/* ---- +QENG servingcell ----
 *
 * ★ 这个报文最坑:LTE 和 NR5G 的字段布局【不一样】,而且中间还夹着
 *   带宽/TAC 之类的字段,偏移不同。必须按 RAT 字段分支,不能一套下标走天下。
 *
 * 真机 NR5G-SA:
 *   +QENG: "servingcell","NOCONN","NR5G-SA","TDD",460,00,123456789,266,
 *          504990,41,5,-81,-10,-73,16,-
 *   下标: 0           1        2          3     4   5   6          7
 *         8(ARFCN)    9(band) 10    11(RSRP) 12(RSRQ) 13(SINR) …
 *
 * Quectel LTE:
 *   +QENG: "servingcell","NOCONN","LTE","FDD",460,00,1234567,266,
 *          1850,3,5,5,1234,-81,-10,-63,16,-
 *   下标: … 8(earfcn) 9(band) 10(ul_bw) 11(dl_bw) 12(TAC)
 *         13(RSRP) 14(RSRQ) 15(RSSI) 16(SINR)
 */
int md_parse_qeng_serving(const char *line, struct md_cell *c)
{
	char tmp[512], *f[24];
	int n;

	if (!line || !c)
		return -1;
	memset(c, 0, sizeof(*c));
	c->arfcn = c->pci = c->cells = -1;
	c->rsrp = c->rsrq = c->sinr = -1000;

	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	n = split_csv(tmp, f, 24);
	if (n < 4)
		return -1;
	if (strcmp(f[0], "servingcell") != 0)
		return -1;

	snprintf(c->rat, sizeof(c->rat), "%s", f[2]);
	c->pci = (n > 7) ? field_long(f[7], -1) : -1;
	c->arfcn = (n > 8) ? field_long(f[8], -1) : -1;
	snprintf(c->band, sizeof(c->band), "%s", (n > 9) ? f[9] : "");

	if (strstr(c->rat, "LTE")) {
		/* LTE:RSRP/RSRQ/RSSI/SINR 在 13/14/15/16 */
		c->rsrp = (n > 13) ? field_long(f[13], -1000) : -1000;
		c->rsrq = (n > 14) ? field_long(f[14], -1000) : -1000;
		c->sinr = (n > 16) ? field_long(f[16], -1000) : -1000;
	} else {
		/* NR5G:RSRP/RSRQ/SINR 在 11/12/13 */
		c->rsrp = (n > 11) ? field_long(f[11], -1000) : -1000;
		c->rsrq = (n > 12) ? field_long(f[12], -1000) : -1000;
		c->sinr = (n > 13) ? field_long(f[13], -1000) : -1000;
	}
	return 0;
}

int md_parse_qeng_neighbour(const char *line, struct md_cell *c)
{
	char tmp[512], *f[24];
	int n;

	if (!line || !c)
		return -1;
	memset(c, 0, sizeof(*c));
	c->arfcn = c->pci = c->cells = -1;
	c->rsrp = c->rsrq = c->sinr = -1000;

	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	n = split_csv(tmp, f, 24);
	/* 邻区行形如:"neighbourcell intra","LTE",1850,266,-81,-10,-63,...
	   有的固件一行给多个小区,每个 4 个字段 —— 只取第一个 */
	if (n < 4 || !strstr(f[0], "neighbourcell"))
		return -1;
	snprintf(c->rat, sizeof(c->rat), "%s", f[1]);
	c->arfcn = field_long(f[2], -1);
	c->pci   = field_long(f[3], -1);
	c->rsrp  = (n > 4) ? field_long(f[4], -1000) : -1000;
	c->rsrq  = (n > 5) ? field_long(f[5], -1000) : -1000;
	c->sinr  = (n > 6) ? field_long(f[6], -1000) : -1000;
	return 0;
}

/* ---- +QTEMP ----
 *
 * 真机是【多行】,每行一个传感器:
 *   +QTEMP: "soc-thermal",54
 *   +QTEMP: "pa-thermal",47
 * 所以调用方要一行一行喂进来。
 */
int md_parse_qtemp(const char *line, long *soc, long *pa)
{
	char tmp[128], *f[4];
	int n;

	if (!line)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s", after_colon(line));
	n = split_csv(tmp, f, 4);
	if (n < 2)
		return -1;
	long v = field_long(f[1], -1000);

	if (v == -1000)
		return -1;
	if (strstr(f[0], "soc"))
		*soc = v;
	else if (strstr(f[0], "pa"))
		*pa = v;
	else
		return -1;
	return 0;
}

/* ---- 纯数字标识(IMEI / IMSI / ICCID)---- */

static int parse_digits(const char *line, char *out, size_t n, int minlen, int maxlen)
{
	const char *p = after_colon(line);
	size_t j = 0;

	if (!out || !n)
		return -1;
	out[0] = '\0';
	for (; *p && j + 1 < n; p++) {
		if (isdigit((unsigned char)*p)) {
			out[j++] = *p;
		} else if (j > 0) {
			break;			/* 数字段结束 */
		}
	}
	out[j] = '\0';
	if ((int)j < minlen || (int)j > maxlen)
		return -1;
	return 0;
}

int md_parse_imei(const char *line, char *out, size_t n)
{
	return parse_digits(line, out, n, 14, 17);
}

int md_parse_imsi(const char *line, char *out, size_t n)
{
	return parse_digits(line, out, n, 14, 16);
}

int md_parse_iccid(const char *line, char *out, size_t n)
{
	/* ICCID 可能 19/20 位,末尾校验位有时缺失 */
	return parse_digits(line, out, n, 18, 22);
}

int md_parse_text(const char *line, char *out, size_t n)
{
	const char *p = after_colon(line);

	if (!out || !n)
		return -1;
	out[0] = '\0';
	if (line && !strchr(line, ':'))
		p = line;			/* 有些固件直接回一行文本 */
	char tmp[MD_STR * 2];

	snprintf(tmp, sizeof(tmp), "%s", p);
	char *s = trim(tmp);

	unquote(s);
	/* 去掉行尾的 OK 之类的尾巴 */
	char *sp = strstr(s, "\r");

	if (sp)
		*sp = '\0';
	snprintf(out, n, "%s", s);
	return out[0] ? 0 : -1;
}

int md_is_5g(const char *network)
{
	if (!network)
		return 0;
	return strstr(network, "NR5G") != NULL ||
	       strstr(network, "NR_") != NULL ||
	       strstr(network, "5G") != NULL;
}
