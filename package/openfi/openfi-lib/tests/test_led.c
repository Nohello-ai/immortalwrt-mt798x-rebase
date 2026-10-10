/* LED 控制测试 —— 造一棵假的 /sys/class/leds 树,验证"按功能名找灯"
 * 这一条特别重要:新 DTS 把灯名从 system/internet/wifi/modem 改成了
 * green:status / green:wan / green:wlan / green:mobile,
 * 如果这层没写对,灯就完全失控 —— 而它现在能在开发机上验证。 */
#define _GNU_SOURCE
#include "../src/openfi_led.h"
#include "../src/openfi_sysfs.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROOT "/tmp/openfi-test-led"

static void mkdirs(const char *rel)
{
	char p[2048];
	size_t i;

	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	for (i = 1; p[i]; i++)
		if (p[i] == '/') { p[i] = '\0'; mkdir(p, 0755); p[i] = '/'; }
	mkdir(p, 0755);
}

static void mkfile(const char *rel, const char *body)
{
	char p[2048], d[1024];
	char *slash;
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	snprintf(d, sizeof(d), "%s", rel);
	slash = strrchr(d, '/');
	if (slash) { *slash = '\0'; mkdirs(d); }
	f = fopen(p, "w");
	if (!f) { perror(p); exit(2); }
	fputs(body, f);
	fclose(f);
}

/* 造一棵和真设备一致的灯树(DTS 用 function/color 命名) */
static void make_tree(void)
{
	const char *leds[] = { "green:status", "green:wan", "green:wlan", "green:mobile" };

	for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
		char p[512];

		snprintf(p, sizeof(p), "sys/class/leds/%s/brightness", leds[i]);
		mkfile(p, "0\n");
		snprintf(p, sizeof(p), "sys/class/leds/%s/trigger", leds[i]);
		mkfile(p, "none timer [none] default-on\n");
		snprintf(p, sizeof(p), "sys/class/leds/%s/delay_on", leds[i]);
		mkfile(p, "0\n");
		snprintf(p, sizeof(p), "sys/class/leds/%s/delay_off", leds[i]);
		mkfile(p, "0\n");
	}
}

int main(void)
{
	char name[128];
	int br, tm;

	mkdir(ROOT, 0755);
	osys_set_root(ROOT);
	make_tree();

	printf("── 按功能名找灯(核心)──\n");
	T_EQ_INT(led_find("status", name, sizeof(name)), 0);
	T_EQ_STR(name, "green:status");
	T_EQ_INT(led_find("wan", name, sizeof(name)), 0);
	T_EQ_STR(name, "green:wan");
	T_EQ_INT(led_find("wlan", name, sizeof(name)), 0);
	T_EQ_STR(name, "green:wlan");
	T_EQ_INT(led_find("mobile", name, sizeof(name)), 0);
	T_EQ_STR(name, "green:mobile");

	printf("── 找不到的要老老实实报错(不能瞎匹配)──\n");
	T_EQ_INT(led_find("nope", name, sizeof(name)), -1);
	T_EQ_INT(led_find("stat", name, sizeof(name)), -1);   /* 不是后缀匹配 */
	T_EQ_INT(led_find("", name, sizeof(name)), -1);
	T_EQ_INT(led_find(NULL, name, sizeof(name)), -1);

	printf("── 亮 / 灭 ──\n");
	T_EQ_INT(led_apply("status", "on", 1, 1000, 1000), 0);
	T_EQ_INT(led_read_state("status", &br, &tm), 0);
	T_EQ_INT(br, 1);
	T_EQ_INT(tm, 0);
	{
		char p[512];
		snprintf(p, sizeof(p), "%s/sys/class/leds/green:status/trigger", ROOT);
		FILE *f = fopen(p, "r");
		char buf[64] = "";
		if (f) { if (!fgets(buf, sizeof(buf), f)) buf[0]=0; fclose(f); }
		T_EQ_STR(buf, "none\n");   /* trigger 被写成 none */
	}
	T_EQ_INT(led_apply("wan", "off", 1, 1000, 1000), 0);
	T_EQ_INT(led_read_state("wan", &br, &tm), 0);
	T_EQ_INT(br, 0);

	printf("── auto:连通→常亮 ──\n");
	T_EQ_INT(led_apply("status", "auto", 1, 1000, 1000), 0);
	T_EQ_INT(led_read_state("status", &br, &tm), 0);
	T_EQ_INT(br, 1);
	T_EQ_INT(tm, 0);

	printf("── auto:不通→闪烁(timer)+ 写了节拍 ──\n");
	T_EQ_INT(led_apply("status", "auto", 0, 700, 300), 0);
	{
		char p[512];
		char v[64] = "";
		FILE *f;

		snprintf(p, sizeof(p), "%s/sys/class/leds/green:status/delay_on", ROOT);
		f = fopen(p, "r"); if (f) { if (!fgets(v, sizeof(v), f)) v[0]=0; fclose(f); }
		T_EQ_STR(v, "700\n");
		snprintf(p, sizeof(p), "%s/sys/class/leds/green:status/delay_off", ROOT);
		v[0] = 0;
		f = fopen(p, "r"); if (f) { if (!fgets(v, sizeof(v), f)) v[0]=0; fclose(f); }
		T_EQ_STR(v, "300\n");
	}

	printf("── keep:什么都不动 ──\n");
	T_EQ_INT(led_apply("wlan", "off", 1, 1000, 1000), 0);
	T_EQ_INT(led_read_state("wlan", &br, &tm), 0);
	T_EQ_INT(br, 0);
	T_EQ_INT(led_apply("wlan", "keep", 1, 1000, 1000), 0);
	T_EQ_INT(led_read_state("wlan", &br, &tm), 0);
	T_EQ_INT(br, 0);                      /* 还是灭的 */

	printf("── 未知模式 / 不存在的灯 ──\n");
	T_EQ_INT(led_apply("wlan", "bogus", 1, 1, 1), -1);
	T_EQ_INT(led_apply("nope", "on", 1, 1, 1), -1);
	T_EQ_INT(led_apply(NULL, "on", 1, 1, 1), -1);
	T_EQ_INT(led_read_state("nope", &br, &tm), -1);

	T_SUMMARY();
}
