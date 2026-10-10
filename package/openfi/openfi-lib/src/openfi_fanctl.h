/* OpenFi 后端公共库 —— 风扇专用部分
 *
 * 曲线本身是通用模板(openfi_curve),这里只做两件风扇特有的事:
 *   ① 把 UCI 里那种「四点 + 上下限」的写法翻译成通用表
 *   ② PWM duty 的反极性换算(本板 0 ns = 全速)
 * 于是"阈值映射"的所有行为都由模板统一提供,不再各处重写。
 */
#ifndef OPENFI_FANCTL_H
#define OPENFI_FANCTL_H

#include "openfi_curve.h"
#include "openfi_temp.h"

/*
 * UCI 里存的就是这四个点和这些限值 —— 保持和配置项一一对应,
 * 免得中间再转一层。
 */
struct fan_cfg {
	int t1, t2, t3, t4;	/* 摄氏度的四个拐点 */
	int s1, s2, s3, s4;	/* 对应的转速 0..100 */
	int min_speed;		/* 转速下限 */
	int emergency_temp;	/* 到这个温度直接满转 */

	/*
	 * 手动模式。
	 *   mode = FAN_MODE_AUTO   按温度曲线
	 *   mode = FAN_MODE_MANUAL 固定 manual_speed
	 * 优先级上紧急温度永远最高(安全),其次是物理静默档,
	 * 然后才是手动 —— 免得用户"固定 0%"把过热保护也一起关掉。
	 */
	int mode;
	int manual_speed;	/* 0..100;仅 mode=MANUAL 时生效 */
};

#define FAN_MODE_AUTO   0
#define FAN_MODE_MANUAL 1

/* 填一份默认配置(与旧版行为一致:55/58/61/65) */
void fan_cfg_default(struct fan_cfg *cfg, int min_speed);

/* 校验:拐点递增、转速单调不降、全在 0..100 */
int fan_cfg_valid(const struct fan_cfg *cfg);

/* 翻译成通用曲线表(STEP 模式,夹取 0..100,带紧急值) */
int fan_cfg_to_curve(const struct fan_cfg *cfg, struct curve *out);

/* PWM duty:反极性换算,period_ns=40000 → 25 kHz。speed 100 = 全速 = duty 0 */
long fan_duty_for(int speed, long period_ns);

/* ================= 风扇状态机(决策全在这里,所以可以离线测透)================= */

/* 为什么是这个转速 —— 界面要显示,日志也要能自证 */
enum fan_reason {
	FAN_R_INIT = 0,		/* 刚启动 */
	FAN_R_CURVE,		/* 按温度曲线 */
	FAN_R_SILENT,		/* 静默模式:停 */
	FAN_R_EMERGENCY,	/* 超过紧急温度:满转 */
	FAN_R_SILENT_HOT,	/* 静默中但过热:强制开(安全兜底) */
	FAN_R_MANUAL,		/* 用户手动指定 */
	FAN_R_HOLD,		/* 迟滞:维持不变 */
};

const char *fan_reason_str(int r);

struct fan_state {
	struct fan_cfg cfg;
	struct curve curve;
	struct curve_hyst hyst;
	int silent;			/* 1 = 物理开关在静默档 */
	int silent_max_temp;		/* 静默时超过它强制开 */
	int last_applied;		/* 上一次下发过的转速, -1 = 还没下发 */
	int last_reason;
};

/*
 * 初始化。silent_max_temp <= 0 时用 75(与原版/6.6 一致)。
 * 返回 0 成功;配置非法返回 -1(调用方应退回 fan_cfg_default)。
 */
int fan_state_init(struct fan_state *st, const struct fan_cfg *cfg, int silent_max_temp);

/* 切静默档(来自开关事件) */
void fan_state_set_silent(struct fan_state *st, int silent);

/*
 * 一步决策。
 *   unified_temp  统一温度(最高那一路),TEMP_INVALID 表示读不到
 *   now_ms        单调时钟毫秒(迟滞用)
 * 返回【需要下发的转速】(0..100),或 -1 表示"这一轮不用动"。
 * 需要下发时,内部已把状态更新好(调用方只管写 PWM)。
 */
int fan_state_step(struct fan_state *st, int unified_temp, long long now_ms);

#endif
