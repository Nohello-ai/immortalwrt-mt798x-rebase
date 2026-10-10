/* OpenFi 后端公共库 —— 温度采集(三路)
 *
 * 三个来源(和原版 op_fan.sh 一致):
 *   CPU    /sys/class/thermal/thermal_zone0/temp      毫摄氏度
 *   WiFi   闭源 MTK 驱动用 iwpriv <iface> stat 暴露 CurrentTemperature
 *   模组   由模组采样进程(luci-app-openfi-modem)每轮写一个整数进文件
 *          —— 走文件而不是自己开 AT 口,省一路串口轮询,也不会跟信号采样抢口
 *
 * 【可测性】解析是纯函数:
 *   · thermal zone 的"毫摄氏度 → 摄氏度"、非法值处理
 *   · iwpriv 输出里的 "CurrentTemperature: 54" 提取(真机格式见过多种变体)
 *   于是开发机上能把解析测透,真机上只剩"文件读不读得到"。
 */
#ifndef OPENFI_TEMP_H
#define OPENFI_TEMP_H

#include <limits.h>

#define TEMP_INVALID INT_MIN

enum temp_src {
	TEMP_CPU = 0,
	TEMP_WIFI,
	TEMP_MODEM,
	TEMP_SRC_COUNT
};

struct temp_cfg {
	char cpu_path[160];
	char wifi_iface[32];
	char modem_file[160];
	int  wifi_enabled;
};

/* 填默认来源路径 */
void temp_cfg_default(struct temp_cfg *c);

/* ---- 纯解析函数(本地可测)---- */

/* 解析 thermal zone 的毫摄氏度字符串 → 摄氏度。非法/超范围返回 TEMP_INVALID */
int temp_parse_millideg(const char *s);

/* 从 iwpriv 的输出里提取 CurrentTemperature。
 * 真机格式形如:
 *     ra0      CurrentTemperature: 54
 *     CurrentTemperature = 54
 * 找不到返回 TEMP_INVALID */
int temp_parse_iwpriv(const char *out);

/* 解析一个"裸整数摄氏度"文件(模组采样文件) */
int temp_parse_celsius(const char *s);

/* ---- 采集 ---- */

/* 读一路。读不到返回 TEMP_INVALID */
int temp_read(enum temp_src src, const struct temp_cfg *cfg);

/* 读三路并算"统一温度"(取读到的里面最高的那个 —— 哪一路最热就以它为准)。
 * out3 可传 NULL。返回成功读到的路数(0..3) */
int temp_read_all(const struct temp_cfg *cfg, int out3[TEMP_SRC_COUNT], int *unified);

#endif
