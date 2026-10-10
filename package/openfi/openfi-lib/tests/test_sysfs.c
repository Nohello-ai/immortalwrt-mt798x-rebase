/* openfi_sysfs 单元测试 —— 用一个假的 sysfs 根目录 */
#define _GNU_SOURCE
#include "../src/openfi_sysfs.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROOT "/tmp/openfi-test-sys"

/* 自动建父目录 */
static void mkdirs(const char *rel)
{
	char p[2048];
	size_t i;

	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	for (i = 1; p[i]; i++) {
		if (p[i] == '/') {
			p[i] = '\0';
			mkdir(p, 0755);
			p[i] = '/';
		}
	}
	mkdir(p, 0755);
}

static void mkfile(const char *rel, const char *body)
{
	char p[2048], dir[1024];
	char *slash;
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	snprintf(dir, sizeof(dir), "%s", rel);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdirs(dir);
	}
	f = fopen(p, "w");
	if (!f) { perror(p); exit(2); }
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	char buf[256];

	mkdir(ROOT, 0755);
	osys_set_root(ROOT);

	/* 造一棵假的 sysfs 树 */
	mkfile("class/thermal/temp", "58000\n");
	mkfile("class/leds/green:status/brightness", "1\n");
	mkfile("class/leds/green:wan/brightness", "0\n");

	printf("── 读 ──\n");
	T_EQ_STR_FREE(osys_read("/class/thermal/temp"), "58000");
	T_EQ_INT(osys_read_int("/class/thermal/temp", -1), 58000);
	T_EQ_INT(osys_read_int("/class/thermal/nope", -1), -1);
	T_OK(osys_read("/class/thermal/nope") == NULL);
	T_EQ_STR_FREE(osys_read("/class/leds/green:status/brightness"), "1");

	printf("── 存在性 ──\n");
	T_OK(osys_exists("/class/leds/green:status") == 1);
	T_OK(osys_exists("/class/leds/green:nope") == 0);

	printf("── 列目录(找灯)──\n");
	T_EQ_INT(osys_list_dir("/class/leds", "green:", buf, sizeof(buf)), 2);
	T_OK(strstr(buf, "green:status") && strstr(buf, "green:wan"));

	printf("── 写 ──\n");
	{
		char p[512];
		snprintf(p, sizeof(p), "%s/class/leds/green:wan/w", ROOT);
		T_OK(osys_write("/class/leds/green:wan/brightness", "7") == 0);
		(void)p;
		T_EQ_STR_FREE(osys_read("/class/leds/green:wan/brightness"), "7");
		T_OK(osys_write_int("/class/leds/green:wan/brightness", 0) == 0);
		T_EQ_STR_FREE(osys_read("/class/leds/green:wan/brightness"), "0");
		T_OK(osys_write("/class/leds/nope/brightness", "1") == -1);
		/* ★ 不存在的文件应当能被创建出来(自己的运行时文件靠这个)。
		   注意父目录得先存在 —— osys_write 只建文件不建目录。 */
		mkdir("/tmp/openfi-test-sys/var", 0755);
		mkdir("/tmp/openfi-test-sys/var/run", 0755);
		T_OK(osys_write("/var/run/newfile", "42") == 0);
		T_EQ_STR_FREE(osys_read("/var/run/newfile"), "42");
		/* 覆盖短值不能留下旧尾巴 */
		T_OK(osys_write("/var/run/newfile", "7") == 0);
		T_EQ_STR_FREE(osys_read("/var/run/newfile"), "7");

		/*
		 * ★ 超长值必须被【拒绝】。
		 *   坑在于 snprintf 返回的是"不截断会写多少字节",不是"实际写了多少" ——
		 *   拿它当长度去 write,就会从 buf 后面多读一截栈内存。
		 *
		 *   ⚠️ 这段必须放在 mkdir(var/run) 之后!第一次我写在前面,
		 *      结果 open() 先失败了,不管有没有 bug 都返回 -1,
		 *      测试"因为错误的原因通过",一点用都没有。
		 */
		{
			char big[600];

			memset(big, '9', sizeof(big) - 1);
			big[sizeof(big) - 1] = '\0';
			T_OK(osys_write("/var/run/bigfile", big) == -1);
			/* 而且要确认它没被写进去半个 */
			T_OK(osys_read("/var/run/bigfile") == NULL);
		}
	}

	T_SUMMARY();
}
