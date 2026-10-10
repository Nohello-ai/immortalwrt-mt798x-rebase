/* OpenFi 后端公共库 —— 物理滑动开关(pio 0 / KEY_CONFIG)
 *
 * 硬件事实(从原厂 DTB 和真机 debugfs 读回来的):
 *   · 开关接在 pio 0,设备树里是 gpio-keys 的 func 节点,
 *     linux,code = <KEY_CONFIG>,ACTIVE_HIGH。
 *   · 内核不提供"读按键当前位置"的接口 —— 事件只在【变化时】推送。
 *     所以想知道"现在在哪一档",只能读 debugfs 里那个引脚的电平:
 *         /sys/kernel/debug/gpio
 *          gpio-512 ( |func ) in  lo IRQ
 *
 * 【为什么不用标志文件】
 *   曾经的做法是:按键事件里写 /var/run/openfi-silent,守护进程读这个文件。
 *   问题是文件只在"事件发生"时才更新 —— 重启后如果没拨过开关,
 *   标志文件是空的,守护进程就以为在正常档,而实际开关可能在静默档。
 *   改成每轮直接读电平,重启后天然正确,也少一个需要同步的状态。
 */
#ifndef OPENFI_SWITCH_H
#define OPENFI_SWITCH_H

/* 开关位置 */
enum sw_level {
	SW_UNKNOWN = -1,
	SW_LOW = 0,
	SW_HIGH = 1,
};

/*
 * 读 debugfs 里 label 为 label(默认 "func")的那个引脚电平。
 * 读不到返回 SW_UNKNOWN(调用方应保持现状,不要乱改)。
 * dbg_path 传 NULL 用默认 /sys/kernel/debug/gpio。
 */
enum sw_level sw_read_level(const char *dbg_path, const char *label);

/* 电平 → 字符串("low"/"high"/"unknown"),给 JSON 输出用 */
const char *sw_level_str(enum sw_level lvl);

/* 电平 → 是否静默。silent_when_hi=1 表示高电平档是静默档。
 * 读不到返回 -1(未知)。 */
int sw_is_silent(enum sw_level lvl, int silent_when_hi);

/* 纯解析:从一整份 /sys/kernel/debug/gpio 内容里取某 label 的电平。
 * 单独抽出来是为了能拿真机抓下来的文本做单元测试。 */
enum sw_level sw_parse(const char *content, const char *label);

#endif
