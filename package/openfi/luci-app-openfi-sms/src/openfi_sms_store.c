#define _GNU_SOURCE
#include "openfi_sms_store.h"
#include "openfi_json.h"
#include "openfi_log.h"
#include "openfi_sysfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_DEFAULT "/etc/openfi-sms"

/* 有界拷贝:源可能比目标长,snprintf 会触发 -Wformat-truncation(我们开了 -Werror)*/
static void copy_str(char *dst, size_t n, const char *src)
{
	size_t l;

	if (!dst || !n)
		return;
	if (!src)
		src = "";
	l = strlen(src);
	if (l >= n)
		l = n - 1;
	memcpy(dst, src, l);
	dst[l] = '\0';
}

static char g_dir[SMS_DIR_MAX] = DIR_DEFAULT;

void sms_store_set_dir(const char *dir)
{
	copy_str(g_dir, sizeof(g_dir), (dir && *dir) ? dir : DIR_DEFAULT);
}

int sms_id_ok(const char *id)
{
	if (!id || !*id || strlen(id) > 64)
		return 0;
	for (const char *p = id; *p; p++)
		if (!((p[0] >= '0' && p[0] <= '9') ||
		      (p[0] >= 'a' && p[0] <= 'z') ||
		      (p[0] >= 'A' && p[0] <= 'Z') ||
		      p[0] == '-' || p[0] == '_'))
			return 0;
	return 1;
}

const char *sms_store_dir(void)
{
	return g_dir;
}

int sms_store_init(void)
{
	struct stat st;

	if (stat(g_dir, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
	/* 逐级建(openfi-sms 只有一级,直接 mkdir 就够)*/
	return mkdir(g_dir, 0755) == 0 ? 0 : -1;
}

/* 路径拼接 */
static void path_of(const char *id, char *out, size_t n)
{
	snprintf(out, n, "%s/%s.json", g_dir, id);
}

/*
 * 生成 id:时间戳 + 一个递增序号,保证同一秒内多条也不会重名。
 * 形如 20261010T153012-3
 */
static void gen_id(char *out, size_t n)
{
	static int seq;
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(out, n, "%04d%02d%02dT%02d%02d%02d-%d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec, seq++);
}

/* 极简 JSON 解析:我们存的文件是自己写的,格式固定,不引第三方解析器 */
static const char *find_field(const char *js, const char *key, char *out, size_t n)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(js, pat);
	if (!p)
		return NULL;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	if (*p == '"') {
		size_t j = 0;

		p++;
		while (*p && *p != '"' && j + 1 < n) {
			if (*p == '\\' && p[1]) {
				p++;
				switch (*p) {
				case 'n': out[j++] = '\n'; break;
				case 't': out[j++] = '\t'; break;
				case 'r': out[j++] = '\r'; break;
				default:  out[j++] = *p; break;
				}
				p++;
			} else {
				out[j++] = *p++;
			}
		}
		out[j] = '\0';
		return out;
	}
	/* 数字/布尔 */
	{
		size_t j = 0;

		while (*p && *p != ',' && *p != '}' && j + 1 < n)
			out[j++] = *p++;
		out[j] = '\0';
		while (j && out[j - 1] == ' ')
			out[--j] = '\0';
	}
	return out;
}

int sms_store_save(struct sms_msg *msg)
{
	char js[SMS_HEX_MAX + 512];
	char tmp[600], fin[560];
	struct ojson *o;
	FILE *f;
	char *rendered;

	if (!msg)
		return -1;
	if (sms_store_init() != 0) {
		olog_err("存储目录 %s 建不出来", g_dir);
		return -1;
	}
	if (!msg->id[0])
		gen_id(msg->id, sizeof(msg->id));
	if (!msg->received_at)
		msg->received_at = (long)time(NULL);

	/* 用同一个 JSON 输出模块,转义之类的行为才一致 */
	o = oj_new();
	oj_str(o, "id", msg->id);
	oj_str(o, "addr", msg->pdu.addr);
	oj_str(o, "text", msg->pdu.text);
	oj_str(o, "ts", msg->pdu.ts);
	oj_str(o, "sca", msg->pdu.sca);
	oj_int(o, "dcs", msg->pdu.dcs);
	oj_int(o, "read", msg->read);
	oj_int(o, "outgoing", msg->outgoing);
	oj_int(o, "received_at", msg->received_at);
	oj_int(o, "concat_total", msg->pdu.concat_total);
	oj_int(o, "concat_seq", msg->pdu.concat_seq);
	oj_int(o, "concat_ref", msg->pdu.concat_ref);
	rendered = oj_render(o);
	oj_free(o);
	if (!rendered)
		return -1;

	snprintf(js, sizeof(js), "%s", rendered);
	free(rendered);

	path_of(msg->id, fin, sizeof(fin));
	snprintf(tmp, sizeof(tmp), "%s.tmp", fin);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "%s\n", js);
	if (fclose(f) != 0)
		return -1;
	/* 原子替换 —— 掉电也不会留下半截文件 */
	return rename(tmp, fin) == 0 ? 0 : -1;
}

/* 从文件内容填出 msg */
static void parse_msg(const char *js, const char *id_hint, struct sms_msg *m)
{
	char buf[SMS_TEXT_MAX];
	char num[32];

	memset(m, 0, sizeof(*m));
	if (id_hint)
		copy_str(m->id, sizeof(m->id), id_hint);
	if (find_field(js, "id", buf, sizeof(buf)))
		copy_str(m->id, sizeof(m->id), buf);
	if (find_field(js, "addr", buf, sizeof(buf)))
		copy_str(m->pdu.addr, sizeof(m->pdu.addr), buf);
	if (find_field(js, "text", buf, sizeof(buf)))
		copy_str(m->pdu.text, sizeof(m->pdu.text), buf);
	if (find_field(js, "ts", buf, sizeof(buf)))
		copy_str(m->pdu.ts, sizeof(m->pdu.ts), buf);
	if (find_field(js, "sca", buf, sizeof(buf)))
		copy_str(m->pdu.sca, sizeof(m->pdu.sca), buf);
	if (find_field(js, "dcs", num, sizeof(num)))
		m->pdu.dcs = (int)strtol(num, NULL, 10);
	if (find_field(js, "read", num, sizeof(num)))
		m->read = (int)strtol(num, NULL, 10);
	if (find_field(js, "outgoing", num, sizeof(num)))
		m->outgoing = (int)strtol(num, NULL, 10);
	if (find_field(js, "received_at", num, sizeof(num)))
		m->received_at = strtol(num, NULL, 10);
	if (find_field(js, "concat_total", num, sizeof(num)))
		m->pdu.concat_total = (int)strtol(num, NULL, 10);
	if (find_field(js, "concat_seq", num, sizeof(num)))
		m->pdu.concat_seq = (int)strtol(num, NULL, 10);
	if (find_field(js, "concat_ref", num, sizeof(num)))
		m->pdu.concat_ref = (int)strtol(num, NULL, 10);
	m->pdu.valid = 1;
}

int sms_store_get(const char *id, struct sms_msg *msg)
{
	char p[512], buf[8192];
	FILE *f;
	size_t n;

	if (!id || !msg)
		return -1;
	path_of(id, p, sizeof(p));
	f = fopen(p, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	parse_msg(buf, id, msg);
	return 0;
}

int sms_store_delete(const char *id)
{
	char p[512];

	if (!id)
		return -1;
	path_of(id, p, sizeof(p));
	return unlink(p) == 0 ? 0 : -1;
}

int sms_store_clear(void)
{
	DIR *d = opendir(g_dir);
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);

		if (l < 6 || strcmp(de->d_name + l - 5, ".json") != 0)
			continue;
		char id[SMS_ID_MAX];

		snprintf(id, sizeof(id), "%.*s", (int)(l - 5), de->d_name);
		if (sms_store_delete(id) == 0)
			n++;
	}
	closedir(d);
	return n;
}

int sms_store_mark(const char *id, int read)
{
	struct sms_msg m;

	if (sms_store_get(id, &m) != 0)
		return -1;
	m.read = read ? 1 : 0;
	return sms_store_save(&m);
}

/* 比较函数:按时间倒序 */
static int cmp_desc(const void *a, const void *b)
{
	const struct sms_msg *x = a, *y = b;

	if (x->received_at != y->received_at)
		return (y->received_at > x->received_at) ? 1 : -1;
	return strcmp(y->id, x->id);
}

int sms_store_list(struct sms_msg *out, int max)
{
	DIR *d = opendir(g_dir);
	struct dirent *de;
	int total = 0;

	if (!d)
		return 0;

	/* 先数一遍总数(用于分页提示),同时收集 */
	static struct sms_msg all[512];

	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);
		char id[SMS_ID_MAX];

		if (l < 6 || strcmp(de->d_name + l - 5, ".json") != 0)
			continue;
		snprintf(id, sizeof(id), "%.*s", (int)(l - 5), de->d_name);
		if (total < (int)(sizeof(all) / sizeof(all[0]))) {
			if (sms_store_get(id, &all[total]) == 0)
				total++;
		} else {
			total++;
		}
	}
	closedir(d);

	/* 实际收进数组的条数(可能少于 total —— 数组满了之后只计数不再收)*/
	{
		int have = total > (int)(sizeof(all) / sizeof(all[0]))
			   ? (int)(sizeof(all) / sizeof(all[0])) : total;

		qsort(all, (size_t)have, sizeof(all[0]), cmp_desc);
	}

	/*
	 * ★ 拷贝条数要同时受三个上界约束:
	 *     ① max  —— 调用方要几条
	 *     ② have —— 我们内部数组实际收了几条
	 *     ③ SMS_STORE_MAX_LIST —— 调用方数组容量的约定值
	 *
	 *   以前只判 ①(max),而 total 是"目录里的文件总数"(可以远大于
	 *   内部数组的 512):库存 520 条 + `list 600` 时 n=520,
	 *   于是既越界读 all[512..519],又往调用方的 512 项数组里越界写。
	 *   函数契约也写清楚了:调用方数组必须至少有 SMS_STORE_MAX_LIST 项。
	 */
	{
		int have = total > (int)(sizeof(all) / sizeof(all[0]))
			 ? (int)(sizeof(all) / sizeof(all[0])) : total;

		if (max > have)
			max = have;
		if (max > SMS_STORE_MAX_LIST)
			max = SMS_STORE_MAX_LIST;
		if (max < 0)
			max = 0;

		for (int i = 0; i < max; i++)
			out[i] = all[i];

		return max;		/* 返回【实际填入的条数】*/
	}
}

int sms_store_unread(void)
{
	static struct sms_msg all[512];
	int n = sms_store_list(all, 512);
	int u = 0;

	for (int i = 0; i < n && i < 512; i++)
		if (!all[i].read)
			u++;
	return u;
}

int sms_store_count(void)
{
	/*
	 * ★ 直接数目录,不要借道 sms_store_list。
	 *   list 的返回值现在是"实际填入调用方数组的条数"(它必须受调用方
	 *   数组大小约束,否则就会越界 —— 那是之前的一个真 bug),
	 *   所以它不再能当"库存总数"用。这里独立数一遍。
	 */
	DIR *d = opendir(g_dir);
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);

		if (l >= 6 && strcmp(de->d_name + l - 5, ".json") == 0)
			n++;
	}
	closedir(d);
	return n;
}
