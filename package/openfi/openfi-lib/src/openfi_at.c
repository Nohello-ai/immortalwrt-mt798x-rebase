#define _GNU_SOURCE
#include "openfi_at.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *rtrim(char *s)
{
	size_t n = strlen(s);

	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

static char *ltrim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	return s;
}

/* 去掉首尾的单/双引号(只去一层) */
static void unquote(char *s)
{
	size_t n = strlen(s);

	if (n >= 2 && ((s[0] == '\'' && s[n - 1] == '\'') ||
		       (s[0] == '"' && s[n - 1] == '"'))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

int at_parse(const char *buf, struct at_resp *r)
{
	char tmp[8192];
	char *save = NULL, *ln;

	if (!buf || !r)
		return -1;
	memset(r, 0, sizeof(*r));
	r->result = AT_RES_TIMEOUT;

	/* 拷贝一份,保留原始行边界(\r\n 都当作分隔) */
	snprintf(tmp, sizeof(tmp), "%s", buf);

	for (ln = strtok_r(tmp, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save)) {
		char *s = ltrim(ln);
		char *t;

		rtrim(s);
		if (!*s)
			continue;
		if (strlen(s) >= AT_LINE_MAX)
			continue;	/* 超长行(比如带很长载荷的 URC)直接丢 */

		/*
		 * 跳过【命令回显】。
		 * 模组默认开着回显(ATE1),发 "AT+CSQ" 会先原样回一行 "AT+CSQ"。
		 * 这行不是数据,留着会让调用方数错行、也可能误当成应答。
		 * 判据:以 AT 开头(大小写不敏感)。
		 * 为什么这个判据安全:真正的响应行要么以 '+' 开头(如 +CSQ:),
		 * 要么是 OK/ERROR;而短信的 PDU 十六进制行以数字开头。
		 */
		if ((s[0] == 'A' || s[0] == 'a') && (s[1] == 'T' || s[1] == 't'))
			continue;

		if (r->n >= AT_MAX_LINES)
			break;

		/* 终止行:解析到这里就结束 */
		if (!strcmp(s, "OK")) {
			r->result = AT_RES_OK;
			break;
		}
		if (!strcmp(s, "ERROR")) {
			r->result = AT_RES_ERROR;
			break;
		}
		if ((t = strstr(s, "+CME ERROR:")) == s || (t = strstr(s, "+CME ERROR:")) == s) {
			r->result = AT_RES_CME;
			r->err_code = (int)strtol(s + strlen("+CME ERROR:"), NULL, 10);
			break;
		}
		if (strstr(s, "+CMS ERROR:") == s) {
			r->result = AT_RES_CMS;
			r->err_code = (int)strtol(s + strlen("+CMS ERROR:"), NULL, 10);
			break;
		}
		snprintf(r->lines[r->n], AT_LINE_MAX, "%s", s);
		r->n++;
	}
	return 0;
}

const char *at_result_str(enum at_result res)
{
	switch (res) {
	case AT_RES_OK:      return "OK";
	case AT_RES_ERROR:   return "ERROR";
	case AT_RES_CME:     return "CME ERROR";
	case AT_RES_CMS:     return "CMS ERROR";
	case AT_RES_TIMEOUT: return "timeout";
	case AT_RES_IO:      return "io error";
	}
	return "unknown";
}

int at_is_ok(const struct at_resp *r)
{
	return r && r->result == AT_RES_OK;
}

const char *at_find(const struct at_resp *r, const char *prefix)
{
	size_t pl;

	if (!r || !prefix)
		return NULL;
	pl = strlen(prefix);
	for (int i = 0; i < r->n; i++)
		if (strncmp(r->lines[i], prefix, pl) == 0)
			return r->lines[i];
	return NULL;
}

/* 把冒号后面的内容按逗号切分。返回字段数。 */
int at_csv(const char *line, char fields[][64], int max_fields)
{
	const char *p;
	int n = 0;

	if (!line || !fields || max_fields <= 0)
		return 0;
	p = strchr(line, ':');
	if (!p)
		return 0;
	p++;
	for (;;) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		char one[128];

		if (len >= sizeof(one))
			len = sizeof(one) - 1;
		memcpy(one, p, len);
		one[len] = '\0';

		char *s = ltrim(one);
		rtrim(s);
		unquote(s);
		if (n < max_fields)
			snprintf(fields[n], 64, "%s", s);
		n++;

		if (!comma || n >= max_fields)
			break;
		p = comma + 1;
	}
	return n;
}

long at_csv_int(const char *line, int idx, long def)
{
	char f[AT_MAX_FIELDS][64];
	int n;

	if (idx < 0)
		return def;
	n = at_csv(line, f, AT_MAX_FIELDS);
	if (idx >= n || !f[idx][0])
		return def;
	char *end;
	long v = strtol(f[idx], &end, 10);

	if (*end)
		return def;
	return v;
}
