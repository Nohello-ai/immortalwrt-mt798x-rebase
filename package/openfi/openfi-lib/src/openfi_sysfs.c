#define _GNU_SOURCE
#include "openfi_sysfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_root = "";

void osys_set_root(const char *root)
{
	g_root = root ? root : "";
}

void osys_path(char *out, size_t outlen, const char *path)
{
	if (path[0] == '/')
		snprintf(out, outlen, "%s%s", g_root, path);
	else
		snprintf(out, outlen, "%s/%s", g_root, path);
}

char *osys_read(const char *path)
{
	char full[512], buf[256];
	FILE *f;
	char *s, *e;

	osys_path(full, sizeof(full), path);
	f = fopen(full, "r");
	if (!f)
		return NULL;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);

	s = buf;
	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return strdup(s);
}

long osys_read_int(const char *path, long def)
{
	char *v = osys_read(path);
	char *end;
	long r;

	if (!v)
		return def;
	r = strtol(v, &end, 10);
	if (*end) {
		free(v);
		return def;
	}
	free(v);
	return r;
}

int osys_write(const char *path, const char *val)
{
	char full[512];
	int fd;
	ssize_t n;
	char buf[256];
	int len;			/* snprintf 的返回值,可能为负 —— 不能用 size_t */

	osys_path(full, sizeof(full), path);
	/*
	 * O_TRUNC:普通文件上必须截断 —— 否则新值比旧值短时旧尾巴会留下
	 * (比如 duty_cycle 从 "28000" 改成 "0",文件会变成 "0\n000\n")。
	 * 对真 sysfs 无害:sysfs 属性忽略 O_TRUNC,内核按本次写入的长度解析。
	 *
	 * O_CREAT:我们自己的运行时文件(如 /var/run/openfi-modem-temp)本来
	 * 不存在,没有这个标志就写不进去 —— 而且失败得很安静,直到发现
	 * "风扇怎么一直拿不到模组温度"才暴露。对已存在的 sysfs 属性,
	 * O_CREAT 是无操作,所以加上它对两边都安全。
	 */
	fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	/*
	 * ★ snprintf 返回的是"假如不截断会写多少字节",不是"实际写了多少"。
	 *   值太长时它会返回一个【大于缓冲区】的数,直接拿它当长度去 write(),
	 *   就会从 buf 后面多读一截栈内存(栈越界读)。返回负值(编码出错)时更糟:
	 *   赋给 size_t 会变成一个巨大的数。
	 *
	 *   而且被截断的值本来也不该写进 sysfs —— 比如 40000 截成 4000,
	 *   会把 PWM 周期悄悄改错。与其写错,不如直接拒绝。
	 */
	len = snprintf(buf, sizeof(buf), "%s\n", val);
	if (len < 0 || (size_t)len >= sizeof(buf))
		return close(fd), -1;
	n = write(fd, buf, (size_t)len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

int osys_write_int(const char *path, long val)
{
	char b[32];

	snprintf(b, sizeof(b), "%ld", val);
	return osys_write(path, b);
}

int osys_exists(const char *path)
{
	char full[512];
	struct stat st;

	osys_path(full, sizeof(full), path);
	return stat(full, &st) == 0;
}

int osys_list_dir(const char *dir, const char *prefix, char *out, size_t outlen)
{
	char full[512];
	DIR *d;
	struct dirent *de;
	size_t plen = prefix ? strlen(prefix) : 0;
	int n = 0, first = 1;

	osys_path(full, sizeof(full), dir);
	d = opendir(full);
	if (!d)
		return 0;
	if (outlen)
		out[0] = '\0';

	while ((de = readdir(d))) {
		int w;

		if (de->d_name[0] == '.')
			continue;
		if (plen && strncmp(de->d_name, prefix, plen) != 0)
			continue;
		w = snprintf(out + strlen(out), outlen - strlen(out),
			     "%s%s", first ? "" : ",", de->d_name);
		if (w < 0 || (size_t)w >= outlen - strlen(out))
			break;
		first = 0;
		n++;
	}
	closedir(d);
	return n;
}
