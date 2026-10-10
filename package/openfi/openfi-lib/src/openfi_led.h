/* OpenFi 后端公共库 —— 状态灯控制
 *
 * 【和 DTS 的约定】
 *   设备树里按 Linux 官方命名法定名(function + color),所以 sysfs 里的目录名是
 *       <颜色>:<功能>      例如  green:status / green:wan / green:wlan / green:mobile
 *   代码里【只认 function 那一段】,颜色用通配匹配 ——
 *   哪天换了灯珠颜色(绿改蓝),这里一行都不用改。
 *
 * 【可测性】
 *   和 sysfs 层一样,root 前缀可注入,测试里造一棵假 leds 树就能完整测。
 */
#ifndef OPENFI_LED_H
#define OPENFI_LED_H

#include <stddef.h>

/*
 * 按功能名找灯。func 传 "status" / "wan" / "wlan" / "mobile"。
 * 找到则把完整目录名(如 "green:wan")写进 out,返回 0;找不到返回 -1。
 */
int led_find(const char *func, char *out, size_t outlen);

/*
 * 设置亮灭/闪烁。
 *   on   常亮
 *   off  常灭
 *   auto 由调用方决定的"连通性"语义:up=1 常亮,否则用内核 timer 闪烁
 *   keep 不干预(什么都不做)
 * blink_on/off 单位毫秒。wan_up 只在 auto 时用。
 * 返回 0 成功, -1 失败(灯不存在或写不进去)
 */
int led_apply(const char *func, const char *mode, int wan_up, int blink_on, int blink_off);

/* 读当前状态。找到返回 0 并填 brightness/timer 是否开启;找不到返回 -1 */
int led_read_state(const char *func, int *brightness, int *timering);

#endif
