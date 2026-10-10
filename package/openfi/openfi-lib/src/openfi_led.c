#define _GNU_SOURCE
#include "openfi_led.h"
#include "openfi_sysfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LED_DIR "/sys/class/leds"

int led_find(const char *func, char *out, size_t outlen)
{
	char list[512];
	char *p;
	int n;

	/* 空串必须挡住:func="" 时拼出来的路径是 "/sys/class/leds/" ——
	   那是个【存在的目录】,会被下面的 osys_exists 误判为"找到了同名灯"。 */
	if (!func || !*func || !out || !outlen)
		return -1;
	out[0] = '\0';

	/* 先看有没有同名(不带颜色前缀)的,兼容 label 风格的固件 */
	char direct[256];

	snprintf(direct, sizeof(direct), "%s/%s", LED_DIR, func);
	if (osys_exists(direct)) {
		snprintf(out, outlen, "%s", func);
		return 0;
	}

	/* 否则在 <颜色>:<功能> 里找 */
	n = osys_list_dir(LED_DIR, NULL, list, sizeof(list));
	if (n <= 0)
		return -1;

	size_t fl = strlen(func);

	for (p = strtok(list, ","); p; p = strtok(NULL, ",")) {
		size_t pl = strlen(p);

		/* 必须形如  <something>:<func> */
		if (pl < fl + 1)
			continue;
		if (strcmp(p + pl - fl, func) != 0)
			continue;
		if (p[pl - fl - 1] != ':')
			continue;
		snprintf(out, outlen, "%s", p);
		return 0;
	}
	return -1;
}

static int led_within(const char *name, const char *what, char *out, size_t outlen)
{
	snprintf(out, outlen, "%s/%s/%s", LED_DIR, name, what);
	return 0;
}

int led_read_state(const char *func, int *brightness, int *timering)
{
	char name[128], path[256];

	if (led_find(func, name, sizeof(name)) != 0)
		return -1;

	led_within(name, "brightness", path, sizeof(path));
	if (brightness) {
		long b = osys_read_int(path, -1);

		if (b < 0)
			return -1;
		*brightness = (int)b;
	}
	if (timering) {
		led_within(name, "trigger", path, sizeof(path));
		char *t = osys_read(path);

		/* trigger 文件形如 "none [timer] ..." —— 方括号里是当前生效的 */
		*timering = 0;
		if (t) {
			char *lb = strchr(t, '[');
			char *rb = lb ? strchr(lb, ']') : NULL;

			if (lb && rb) {
				*lb++ = '\0';
				*rb = '\0';
				*timering = strcmp(lb, "timer") == 0;
			}
			free(t);
		}
	}
	return 0;
}

int led_apply(const char *func, const char *mode, int wan_up, int blink_on, int blink_off)
{
	char name[128], path[256];

	if (!func || !mode)
		return -1;
	if (led_find(func, name, sizeof(name)) != 0)
		return -1;

	if (!strcmp(mode, "keep"))
		return 0;

	if (!strcmp(mode, "on") || !strcmp(mode, "off")) {
		led_within(name, "trigger", path, sizeof(path));
		osys_write(path, "none");
		led_within(name, "brightness", path, sizeof(path));
		return osys_write(path, !strcmp(mode, "on") ? "1" : "0");
	}

	if (!strcmp(mode, "auto")) {
		if (wan_up) {
			led_within(name, "trigger", path, sizeof(path));
			osys_write(path, "none");
			led_within(name, "brightness", path, sizeof(path));
			return osys_write(path, "1");
		}
		/* 不通 → 用内核 timer 闪,不占用户态进程 */
		char b[32];

		led_within(name, "delay_off", path, sizeof(path));
		snprintf(b, sizeof(b), "%d", blink_off > 0 ? blink_off : 1000);
		osys_write(path, b);
		led_within(name, "delay_on", path, sizeof(path));
		snprintf(b, sizeof(b), "%d", blink_on > 0 ? blink_on : 1000);
		osys_write(path, b);
		led_within(name, "trigger", path, sizeof(path));
		return osys_write(path, "timer");
	}

	return -1;
}
