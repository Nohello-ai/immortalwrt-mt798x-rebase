/* OpenFi 后端公共库 —— 通用"分段阈值映射"模板
 *
 * 这是一个【到处都用得上】的模式:x 落在哪个区间,输出对应的 y。
 *   · 温度 → 风扇转速
 *   · RSRP/SINR → 信号格数(0..5)
 *   · 温度 → 告警级别
 *   · 任意数值 → 界面上的百分比
 *
 * 所以做成数据驱动的一张表(点数不限),而不是每个功能各写一套 if-else。
 * 好处:
 *   · 阈值可以从 UCI 读出来,用户想改就改,不用改代码
 *   · 校验、夹取、紧急值、迟滞只有一份实现,行为一致
 *   · 点数是数据,想从 4 点变 6 点不用动逻辑
 *
 * 两种取值模式:
 *   CURVE_STEP   x 落在区间里 → 取该区间的 y(阶梯,旧版风扇曲线就是这种)
 *   CURVE_LINEAR 两点之间线性插值(适合做平滑的效果)
 */
#ifndef OPENFI_CURVE_H
#define OPENFI_CURVE_H

#define CURVE_MAX_POINTS 16

enum curve_mode {
	CURVE_STEP = 0,
	CURVE_LINEAR,
};

struct curve_point {
	int x;
	int y;
};

struct curve {
	struct curve_point pt[CURVE_MAX_POINTS];
	int n;
	enum curve_mode mode;
	int min_y, max_y;	/* 输出夹取范围 */
	int emerg_x;		/* x >= emerg_x → 直接给 emerg_y(0 = 不用) */
	int emerg_y;
};

/*
 * 装载一张表。pts 必须按 x 升序。
 * min_y/max_y 用于夹取;emerg_x=0 表示不设紧急值。
 * 返回 0 成功, -1 参数错。
 */
int curve_load(struct curve *c, const struct curve_point *pts, int n,
	       enum curve_mode mode, int min_y, int max_y, int emerg_x, int emerg_y);

/*
 * 校验:点数 ≥ 2、x 严格递增、y 单调不减、夹取范围合法。
 * 不合法返回 0(调用方应退回内置默认表)。
 */
int curve_valid(const struct curve *c);

/*
 * 查询。
 *   x <= pt[0].x        → pt[0].y
 *   落在 pt[i], pt[i+1] → STEP 取 pt[i].y;LINEAR 插值
 *   x >  pt[n-1].x      → pt[n-1].y
 * 结果一定落在 [min_y, max_y] 内。
 */
int curve_eval(const struct curve *c, int x);

/*
 * 迟滞:避免输入在阈值附近抖动时输出一直变。
 *   · 输出增大(变热/变强)→ 立刻生效
 *   · 输出减小 → 差值要 ≥ min_delta,且距上次调整 ≥ interval_ms
 */
struct curve_hyst {
	int last_y;
	long long last_ms;
	int min_delta;
	long interval_ms;
};

void curve_hyst_init(struct curve_hyst *h, int min_delta, long interval_ms);
int  curve_hyst_should_apply(struct curve_hyst *h, int target_y, long long now_ms);
void curve_hyst_applied(struct curve_hyst *h, int y, long long now_ms);

/*
 * 小工具:省得每个调用方都手写 struct curve_point 数组、还得数点数。
 *
 *   CURVE_LOAD(&c, CURVE_STEP, 0, 100, 85, 100,
 *              {55,5},{58,36},{61,68},{65,100});
 *
 * 点数由 sizeof 自动算出来 —— 加一个点不用改数字。
 */
#define CURVE_PTS(...) ((const struct curve_point[]){ __VA_ARGS__ })

#define CURVE_LOAD(dst, mode, miny, maxy, ex, ey, ...)			\
	curve_load((dst), CURVE_PTS(__VA_ARGS__),			\
		   (int)(sizeof(CURVE_PTS(__VA_ARGS__)) /		\
			 sizeof(struct curve_point)),			\
		   (mode), (miny), (maxy), (ex), (ey))

#endif
