/* 风扇适配器测试 —— 曲线行为由通用模板提供,这里测"翻译"和 PWM 换算 */
#define _GNU_SOURCE
#include "../src/openfi_fanctl.h"
#include "t.h"

#include <stdio.h>

int main(void)
{
	struct fan_cfg cfg;
	struct curve c;

	printf("── 默认配置 ──\n");
	fan_cfg_default(&cfg, 5);
	T_EQ_INT(cfg.t1, 55); T_EQ_INT(cfg.t2, 58);
	T_EQ_INT(cfg.t3, 61); T_EQ_INT(cfg.t4, 65);
	T_EQ_INT(cfg.s1, 5);  T_EQ_INT(cfg.s4, 100);
	T_EQ_INT(cfg.emergency_temp, 85);
	T_EQ_INT(fan_cfg_valid(&cfg), 1);

	printf("── 翻译成通用表后取值一致 ──\n");
	T_EQ_INT(fan_cfg_to_curve(&cfg, &c), 0);
	T_EQ_INT(c.n, 4);
	T_EQ_INT(c.mode, CURVE_STEP);
	T_EQ_INT(c.min_y, 5);
	T_EQ_INT(c.max_y, 100);
	T_EQ_INT(c.emerg_x, 85);
	T_EQ_INT(curve_eval(&c, 20), cfg.s1);
	T_EQ_INT(curve_eval(&c, 55), cfg.s1);
	T_EQ_INT(curve_eval(&c, 56), cfg.s2);
	T_EQ_INT(curve_eval(&c, 58), cfg.s2);
	T_EQ_INT(curve_eval(&c, 59), cfg.s3);
	T_EQ_INT(curve_eval(&c, 62), cfg.s4);
	T_EQ_INT(curve_eval(&c, 84), cfg.s4);
	T_EQ_INT(curve_eval(&c, 85), 100);     /* 紧急 */

	printf("── 非法配置被拒 ──\n");
	fan_cfg_default(&cfg, 5);
	{
		struct fan_cfg bad = cfg;
		bad.t2 = bad.t1 + 1;               /* 拐点间距 < 2 */
		T_EQ_INT(fan_cfg_valid(&bad), 0);
		bad = cfg; bad.s3 = bad.s1 - 1;    /* 转速下降 */
		T_EQ_INT(fan_cfg_valid(&bad), 0);
		bad = cfg; bad.s4 = 101;           /* 越界 */
		T_EQ_INT(fan_cfg_valid(&bad), 0);
		bad = cfg; bad.emergency_temp = bad.t4;  /* 紧急值没在最后点之后 */
		T_EQ_INT(fan_cfg_valid(&bad), 0);
		bad = cfg; bad.min_speed = -1;
		T_EQ_INT(fan_cfg_valid(&bad), 0);
		bad = cfg; bad.t4 = 90;            /* t4 超过紧急温度 */
		T_EQ_INT(fan_cfg_valid(&bad), 0);
	}
	T_EQ_INT(fan_cfg_valid(NULL), 0);
	T_EQ_INT(fan_cfg_to_curve(NULL, &c), -1);
	T_EQ_INT(fan_cfg_to_curve(&cfg, NULL), -1);

	printf("── 下限不同的配置(type 位) ──\n");
	fan_cfg_default(&cfg, 40);
	T_EQ_INT(fan_cfg_valid(&cfg), 1);
	T_EQ_INT(fan_cfg_to_curve(&cfg, &c), 0);
	T_EQ_INT(curve_eval(&c, 20), 40);      /* 下限生效 */

	printf("── PWM duty 反极性(0 ns = 全速)──\n");
	T_EQ_INT(fan_duty_for(100, 40000), 0);
	T_EQ_INT(fan_duty_for(0, 40000), 40000);
	T_EQ_INT(fan_duty_for(50, 40000), 20000);
	T_EQ_INT(fan_duty_for(25, 40000), 30000);
	T_EQ_INT(fan_duty_for(-5, 40000), 40000);   /* 夹取 */
	T_EQ_INT(fan_duty_for(200, 40000), 0);
	T_EQ_INT(fan_duty_for(100, 0), 0);

	printf("── 边界:空指针不崩 ──\n");
	fan_cfg_default(NULL, 5);

	T_SUMMARY();
}
