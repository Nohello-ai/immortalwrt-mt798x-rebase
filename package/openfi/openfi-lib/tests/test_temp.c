/* 温度采集测试 —— 解析用真机见过的格式,采集用假文件 */
#define _GNU_SOURCE
#include "../src/openfi_temp.h"
#include "../src/openfi_sysfs.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROOT "/tmp/openfi-test-temp"

/* 递归建父目录,再写文件 */
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
	char *s;
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	snprintf(d, sizeof(d), "%s", rel);
	s = strrchr(d, '/');
	if (s) { *s = '\0'; mkdirs(d); }
	f = fopen(p, "w");
	if (!f) { perror(p); exit(2); }
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	struct temp_cfg cfg;
	int out3[TEMP_SRC_COUNT], unified;

	printf("── thermal zone 毫摄氏度解析 ──\n");
	T_EQ_INT(temp_parse_millideg("58000\n"), 58);
	T_EQ_INT(temp_parse_millideg("58000"), 58);
	T_EQ_INT(temp_parse_millideg("  47000  "), 47);
	T_EQ_INT(temp_parse_millideg("54600\n"), 54);
	T_EQ_INT(temp_parse_millideg("-5000"), -5);
	T_EQ_INT(temp_parse_millideg("0"), 0);
	/* 非法/越界 */
	T_EQ_INT(temp_parse_millideg(""), TEMP_INVALID);
	T_EQ_INT(temp_parse_millideg("abc"), TEMP_INVALID);
	T_EQ_INT(temp_parse_millideg("999999"), TEMP_INVALID);   /* 超 150℃ */
	T_EQ_INT(temp_parse_millideg("-999999"), TEMP_INVALID);
	T_EQ_INT(temp_parse_millideg(NULL), TEMP_INVALID);

	printf("── iwpriv 输出解析(真机见过的几种写法)──\n");
	T_EQ_INT(temp_parse_iwpriv("ra0      CurrentTemperature: 54\n"), 54);
	T_EQ_INT(temp_parse_iwpriv("CurrentTemperature = 61\n"), 61);
	T_EQ_INT(temp_parse_iwpriv("CurrentTemperature:47"), 47);
	T_EQ_INT(temp_parse_iwpriv("ra0 CurrentTemperature:  52  \n"
				  "ra0 CurrentTemperature: 99\n"), 52);  /* 取第一个 */
	T_EQ_INT(temp_parse_iwpriv("something else\n"), TEMP_INVALID);
	T_EQ_INT(temp_parse_iwpriv("CurrentTemperatureXyz: 5"), TEMP_INVALID); /* 前缀相似但不是 */
	T_EQ_INT(temp_parse_iwpriv("CurrentTemperature:"), TEMP_INVALID);
	T_EQ_INT(temp_parse_iwpriv(NULL), TEMP_INVALID);

	printf("── 裸摄氏度文件解析 ──\n");
	T_EQ_INT(temp_parse_celsius("47\n"), 47);
	T_EQ_INT(temp_parse_celsius("54"), 54);
	T_EQ_INT(temp_parse_celsius("-10"), -10);
	T_EQ_INT(temp_parse_celsius(""), TEMP_INVALID);
	T_EQ_INT(temp_parse_celsius("999"), TEMP_INVALID);
	T_EQ_INT(temp_parse_celsius("x"), TEMP_INVALID);

	printf("── 默认配置 ──\n");
	temp_cfg_default(&cfg);
	T_EQ_STR(cfg.cpu_path, "/sys/class/thermal/thermal_zone0/temp");
	T_EQ_STR(cfg.wifi_iface, "ra0");
	T_EQ_STR(cfg.modem_file, "/var/run/openfi-modem-temp");
	T_EQ_INT(cfg.wifi_enabled, 1);

	printf("── 三路采集 + 统一温度(取最高)──\n");
	mkdir(ROOT, 0755);
	osys_set_root(ROOT);
	mkfile("sys/class/thermal/thermal_zone0/temp", "52000\n");
	mkfile("var/run/openfi-modem-temp", "47\n");
	cfg.wifi_enabled = 0;                    /* 本地没法跑 iwpriv */
	snprintf(cfg.cpu_path, sizeof(cfg.cpu_path),
		 "/sys/class/thermal/thermal_zone0/temp");
	snprintf(cfg.modem_file, sizeof(cfg.modem_file), "/var/run/openfi-modem-temp");

	T_EQ_INT(temp_read(TEMP_CPU, &cfg), 52);
	T_EQ_INT(temp_read(TEMP_MODEM, &cfg), 47);
	T_EQ_INT(temp_read(TEMP_WIFI, &cfg), TEMP_INVALID);   /* 关掉了 */

	T_EQ_INT(temp_read_all(&cfg, out3, &unified), 2);     /* 读到 2 路 */
	T_EQ_INT(out3[TEMP_CPU], 52);
	T_EQ_INT(out3[TEMP_MODEM], 47);
	T_EQ_INT(out3[TEMP_WIFI], TEMP_INVALID);
	T_EQ_INT(unified, 52);                                /* 取最高 */

	printf("── 温度下降后统一温度跟着变 ──\n");
	mkfile("sys/class/thermal/thermal_zone0/temp", "44000\n");
	mkfile("var/run/openfi-modem-temp", "51\n");
	T_EQ_INT(temp_read_all(&cfg, out3, &unified), 2);
	T_EQ_INT(unified, 51);                                /* 这次是模组最高 */

	printf("── 全都读不到时 ──\n");
	cfg.cpu_path[0] = '\0';
	cfg.modem_file[0] = '\0';
	T_EQ_INT(temp_read_all(&cfg, out3, &unified), 0);
	T_EQ_INT(unified, TEMP_INVALID);

	printf("── 空指针不崩 ──\n");
	temp_cfg_default(NULL);
	T_EQ_INT(temp_read(TEMP_CPU, NULL), TEMP_INVALID);
	T_EQ_INT(temp_read_all(NULL, NULL, NULL), 0);

	T_SUMMARY();
}
