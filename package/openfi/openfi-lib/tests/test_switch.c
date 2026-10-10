/* 开关解析测试 —— 用真机抓下来的那份 debugfs 文本 */
#define _GNU_SOURCE
#include "../src/openfi_switch.h"
#include "../src/openfi_sysfs.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ★ 真机 cat /sys/kernel/debug/gpio 的原样输出(开关在 lo) */
static const char *REAL_LO =
"gpiochip0: GPIOs 512-568, parent: platform/11d00000.pinctrl, pinctrl_moore:\n"
" gpio-512 (                    |func                ) in  lo IRQ \n"
" gpio-513 (                    |reset               ) in  hi IRQ ACTIVE LOW\n"
" gpio-514 (                    |system              ) out lo ACTIVE LOW\n"
" gpio-520 (                    |hub_reset           ) out lo ACTIVE LOW\n"
" gpio-521 (                    |lte_power           ) out hi ACTIVE LOW\n"
" gpio-522 (                    |lte_reset           ) out hi ACTIVE LOW\n"
" gpio-523 (                    |modem               ) out lo ACTIVE LOW\n"
" gpio-524 (                    |lte_reset_2         ) out lo \n"
" gpio-546 (                    |internet            ) out lo ACTIVE LOW\n"
" gpio-547 (                    |wifi                ) out lo ACTIVE LOW\n";

/* 同一份,但开关拨到 hi */
static const char *REAL_HI =
"gpiochip0: GPIOs 512-568, parent: platform/11d00000.pinctrl, pinctrl_moore:\n"
" gpio-512 (                    |func                ) in  hi IRQ \n"
" gpio-513 (                    |reset               ) in  hi IRQ ACTIVE LOW\n"
" gpio-514 (                    |system              ) out lo ACTIVE LOW\n";

static void mkfile(const char *rel, const char *body)
{
	char p[1024];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", "/tmp/openfi-test-sw", rel);
	f = fopen(p, "w");
	if (!f) { perror(p); exit(2); }
	fputs(body, f);
	fclose(f);
}

int main(void)
{
	printf("── 真机文本解析 ──\n");
	T_EQ_INT(sw_parse(REAL_LO, "func"), SW_LOW);
	T_EQ_INT(sw_parse(REAL_HI, "func"), SW_HIGH);
	T_EQ_INT(sw_parse(REAL_LO, "reset"), SW_HIGH);   /* 按键也是输入 */
	T_EQ_INT(sw_parse(REAL_LO, "system"), SW_UNKNOWN); /* 输出脚不算 */
	T_EQ_INT(sw_parse(REAL_LO, "modem"), SW_UNKNOWN);

	printf("── 不能瞎匹配 ──\n");
	T_EQ_INT(sw_parse(REAL_LO, "fun"), SW_UNKNOWN);      /* 不能前缀匹配 */
	T_EQ_INT(sw_parse(REAL_LO, "func2"), SW_UNKNOWN);
	T_EQ_INT(sw_parse(REAL_LO, "nope"), SW_UNKNOWN);
	T_EQ_INT(sw_parse("", "func"), SW_UNKNOWN);
	T_EQ_INT(sw_parse(NULL, "func"), SW_UNKNOWN);
	T_EQ_INT(sw_parse(REAL_LO, NULL), SW_UNKNOWN);
	T_EQ_INT(sw_parse(REAL_LO, ""), SW_UNKNOWN);
	/* 名字前面也像的干扰项 */
	T_EQ_INT(sw_parse(" gpio-500 ( |xfunc ) in hi IRQ\n"
			  " gpio-512 ( |func ) in  lo IRQ\n", "func"), SW_LOW);

	printf("── 静默判定 ──\n");
	T_EQ_INT(sw_is_silent(SW_HIGH, 1), 1);   /* 默认:高电平是静默档 */
	T_EQ_INT(sw_is_silent(SW_LOW, 1), 0);
	T_EQ_INT(sw_is_silent(SW_HIGH, 0), 0);   /* 反过来配 */
	T_EQ_INT(sw_is_silent(SW_LOW, 0), 1);
	T_EQ_INT(sw_is_silent(SW_UNKNOWN, 1), -1);  /* 未知 → 不要乱改 */

	printf("── 从文件读(走 root 注入)──\n");
	mkdir("/tmp/openfi-test-sw", 0755);
	mkdir("/tmp/openfi-test-sw/sys", 0755);
	mkdir("/tmp/openfi-test-sw/sys/kernel", 0755);
	mkdir("/tmp/openfi-test-sw/sys/kernel/debug", 0755);
	osys_set_root("/tmp/openfi-test-sw");
	mkfile("sys/kernel/debug/gpio", REAL_LO);
	T_EQ_INT(sw_read_level("/sys/kernel/debug/gpio", "func"), SW_LOW);
	mkfile("sys/kernel/debug/gpio", REAL_HI);
	T_EQ_INT(sw_read_level("/sys/kernel/debug/gpio", "func"), SW_HIGH);
	mkfile("sys/kernel/debug/gpio", "什么都没有\n");
	T_EQ_INT(sw_read_level("/sys/kernel/debug/gpio", "func"), SW_UNKNOWN);
	T_EQ_INT(sw_read_level("/不存在/gpio", "func"), SW_UNKNOWN);  /* 文件没有 → 未知 */

	T_SUMMARY();
}
