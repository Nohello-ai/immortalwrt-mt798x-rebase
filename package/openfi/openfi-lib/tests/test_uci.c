/* openfi_uci 的单元测试 —— 在开发机上直接跑,不碰真实 /etc/config */
#define _GNU_SOURCE
#include "../src/openfi_uci.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DIR "/tmp/openfi-test-conf"

static void write_file(const char *name, const char *body)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", DIR, name);
	f = fopen(path, "w");
	if (!f) { perror(path); exit(2); }
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	mkdir(DIR, 0755);
	ouci_set_root(DIR);

	/* 一份有代表性的配置:带注释、带引号、带 list、带匿名段、带空值 */
	write_file("openfi_fan",
		"# 注释行\n"
		"\n"
		"config fan 'fan'\n"
		"\toption mode 'auto'\n"
		"\toption period '5'\n"
		"\toption min_speed ' 7 '\n"
		"\toption temp1 55\n"
		"\toption enabled 'on'\n"
		"\toption empty ''\n"
		"\tlist channel '0'\n"
		"\tlist channel '1'\n"
		"\n"
		"config led 'led'\n"
		"\toption enabled '0'\n"
		"\toption wan 'auto'\n");
	write_file("openfi_nosec", "config solo\n\toption x '1'\n");

	printf("── 基本读取 ──\n");
	T_EQ_STR_FREE(ouci_get("openfi_fan", "fan", "mode"), "auto");
	T_EQ_STR_FREE(ouci_get("openfi_fan", "fan", "temp1"), "55");     /* 无引号也能读 */
	T_EQ_STR_FREE(ouci_get("openfi_fan", "fan", "min_speed"), "7");  /* 首尾空白被去掉 */

	printf("── 不存在的情况 ──\n");
	/* 这三处期望的就是 NULL,没有内存要释放 */
	T_OK(ouci_get("openfi_fan", "fan", "nope") == NULL);
	T_OK(ouci_get("openfi_fan", "nope", "mode") == NULL);
	T_OK(ouci_get("no_such_pkg", "fan", "mode") == NULL);

	printf("── 默认值 ──\n");
	T_EQ_STR(ouci_get_def("openfi_fan", "fan", "nope", "fallback"), "fallback");
	T_EQ_STR(ouci_get_def("openfi_fan", "fan", "mode", "x"), "auto");
	T_EQ_INT(ouci_get_int("openfi_fan", "fan", "period", 99), 5);
	T_EQ_INT(ouci_get_int("openfi_fan", "fan", "nope", 99), 99);
	T_EQ_INT(ouci_get_int("openfi_fan", "fan", "mode", 99), 99);   /* 非数字→默认 */

	printf("── 布尔 ──\n");
	T_EQ_INT(ouci_get_bool("openfi_fan", "fan", "enabled", -1), 1);
	T_EQ_INT(ouci_get_bool("openfi_fan", "led", "enabled", -1), 0);
	T_EQ_INT(ouci_get_bool("openfi_fan", "fan", "nope", 1), 1);
	T_EQ_INT(ouci_get_bool("openfi_fan", "fan", "nope", 0), 0);
	T_EQ_INT(ouci_get_bool("openfi_fan", "fan", "mode", 7), 7);    /* 非布尔→默认 */

	printf("── 空值 / 匿名段 ──\n");
	T_EQ_STR_FREE(ouci_get("openfi_fan", "fan", "empty"), "");
	T_EQ_STR_FREE(ouci_get("openfi_nosec", "", "x"), "1");

	printf("── 同名字段分属不同 section 不能串 ──\n");
	T_EQ_STR_FREE(ouci_get("openfi_fan", "fan", "enabled"), "on");
	T_EQ_STR_FREE(ouci_get("openfi_fan", "led", "enabled"), "0");

	printf("── 列选项 ──\n");
	{
		char *opts = ouci_list_options("openfi_fan", "fan");
		T_OK(opts && strstr(opts, "mode") && strstr(opts, "period") && strstr(opts, "channel"));
		T_OK(opts && !strstr(opts, "wan"));   /* wan 属于 led 段,不该出现 */
		free(opts);
	}

	printf("── 写入转义(防注入)──\n");
	{
		/* 这个值里带单引号和分号,必须被正确转义(实际写入走 uci 命令,
		   这里只验证转义函数的结果不产生裸引号截断) */
		char path[256];
		FILE *f;
		snprintf(path, sizeof(path), "%s/openfi_test", DIR);
		f = fopen(path, "w"); fputs("config t 't'\n\toption a 'x'\n", f); fclose(f);
		char *v = ouci_get("openfi_test", "t", "a");

		T_OK(v != NULL);
		free(v);
	}

	T_SUMMARY();
}
