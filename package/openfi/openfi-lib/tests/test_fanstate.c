/* 风扇状态机测试 —— 这是"守护进程到底怎么决策"的全部逻辑,离线测透 */
#define _GNU_SOURCE
#include "../src/openfi_fanctl.h"
#include "../src/openfi_temp.h"
#include "t.h"

#include <stdio.h>

int main(void)
{
	struct fan_cfg cfg;
	struct fan_state st;
	long long t = 1000;

	printf("── 初始化 ──\n");
	fan_cfg_default(&cfg, 10);
	T_EQ_INT(fan_state_init(&st, &cfg, 0), 0);
	T_EQ_INT(st.silent, 0);
	T_EQ_INT(st.silent_max_temp, 75);      /* 默认兜底温度 */
	T_EQ_INT(st.last_applied, -1);
	T_EQ_INT(fan_state_init(&st, NULL, 0), -1);
	{
		struct fan_cfg bad = cfg;
		bad.t2 = bad.t1;                    /* 非法 */
		T_EQ_INT(fan_state_init(&st, &bad, 0), -1);
	}
	fan_state_init(&st, &cfg, 70);
	T_EQ_INT(st.silent_max_temp, 70);

	printf("── 正常曲线 ──\n");
	fan_state_init(&st, &cfg, 75);
	/* 默认曲线 55/58/61/65 → 10/40/70/100 */
	T_EQ_INT(fan_state_step(&st, 30, t += 1000), 10);   /* ≤t1 → s1 */
	T_EQ_INT(st.last_reason, FAN_R_CURVE);
	T_EQ_INT(fan_state_step(&st, 56, t += 1000), 40);   /* 升速立刻 */
	T_EQ_INT(fan_state_step(&st, 62, t += 1000), 100);
	T_EQ_INT(st.last_applied, 100);

	printf("── 迟滞:小幅回落不动 ──\n");
	/* 现在 100;降到 70 想回落,差值 30 > min_delta 5,但间隔可能不够 */
	t += 1000;
	T_EQ_INT(fan_state_step(&st, 60, t), -1);            /* 距上次 1s < 10s → 不动 */
	T_EQ_INT(st.last_reason, FAN_R_HOLD);
	T_EQ_INT(fan_state_step(&st, 60, t += 11000), 70);   /* 过了 10s → 下发 */

	printf("── 迟滞:重复同值不下发 ──\n");
	t += 11000;
	T_EQ_INT(fan_state_step(&st, 60, t), -1);            /* 目标还是 70,相同 */

	printf("── 紧急温度:不管迟滞立刻满转 ──\n");
	T_EQ_INT(fan_state_step(&st, 90, t += 1000), 100);
	T_EQ_INT(st.last_reason, FAN_R_EMERGENCY);

	printf("── ★ 边界:正好等于紧急温度就要满转(>= 不是 >)──\n");
	/*
	 * 变异测试发现:原来只用 90℃ 测(离 85 很远),把 >= 改成 > 也抓不到。
	 * 紧急保护这种东西恰恰是"差一度"要命,必须卡在边界上测。
	 */
	{
		struct fan_state b;

		fan_state_init(&b, &cfg, 75);
		T_EQ_INT(fan_state_step(&b, cfg.emergency_temp, 1000), 100);
		T_EQ_INT(b.last_reason, FAN_R_EMERGENCY);

		fan_state_init(&b, &cfg, 75);
		T_EQ_INT(fan_state_step(&b, cfg.emergency_temp - 1, 1000),
			 b.cfg.s4);              /* 差一度就按曲线走 */
		T_EQ_INT(b.last_reason, FAN_R_CURVE);
	}

	printf("── 静默模式:停 ──\n");
	fan_state_set_silent(&st, 1);
	T_EQ_INT(fan_state_step(&st, 40, t += 1000), 0);
	T_EQ_INT(st.last_reason, FAN_R_SILENT);
	T_EQ_INT(st.last_applied, 0);

	printf("── 静默中过热:安全兜底强制开 ──\n");
	/* 76℃ 已经超过曲线最后一个拐点 t4=65 → 曲线本身就给满速 100 */
	T_EQ_INT(fan_state_step(&st, 76, t += 1000), 100);
	T_EQ_INT(st.last_reason, FAN_R_SILENT_HOT);
	T_EQ_INT(fan_state_step(&st, 63, t += 1000), 0);     /* 降到拐点以下 → 又停 */
	T_EQ_INT(st.last_reason, FAN_R_SILENT);
	T_EQ_INT(fan_state_step(&st, 76, t += 1000), 100);   /* 再回到 76 */
	T_EQ_INT(st.last_reason, FAN_R_SILENT_HOT);
	printf("── ★ 边界:正好等于静默兜底温度就要强制开(>= 不是 >)──\n");
	{
		struct fan_state b;

		fan_state_init(&b, &cfg, 75);
		fan_state_set_silent(&b, 1);
		T_EQ_INT(fan_state_step(&b, 75, 1000), b.cfg.s4);
		T_EQ_INT(b.last_reason, FAN_R_SILENT_HOT);

		fan_state_init(&b, &cfg, 75);
		fan_state_set_silent(&b, 1);
		T_EQ_INT(fan_state_step(&b, 74, 1000), 0);      /* 差一度就继续停 */
		T_EQ_INT(b.last_reason, FAN_R_SILENT);
	}

	printf("── 静默中到紧急温度:拉满(优先级最高)──\n");
	T_EQ_INT(fan_state_step(&st, 90, t += 1000), 100);
	T_EQ_INT(st.last_reason, FAN_R_EMERGENCY);

	printf("── 拨回正常档:立刻恢复 ──\n");
	fan_state_set_silent(&st, 0);
	T_EQ_INT(fan_state_step(&st, 56, t += 1000), 40);
	T_EQ_INT(st.last_reason, FAN_R_CURVE);

	printf("── 温度读不到 ──\n");
	fan_state_init(&st, &cfg, 75);
	T_EQ_INT(fan_state_step(&st, TEMP_INVALID, t), 50);  /* 首次给安全中速 */
	T_EQ_INT(st.last_reason, FAN_R_INIT);
	T_EQ_INT(fan_state_step(&st, TEMP_INVALID, t += 1000), -1);  /* 之后保持不动 */
	T_EQ_INT(st.last_reason, FAN_R_HOLD);

	printf("── 原因字符串(界面/日志要用)──\n");
	T_EQ_STR(fan_reason_str(FAN_R_CURVE), "curve");
	T_EQ_STR(fan_reason_str(FAN_R_SILENT), "silent");
	T_EQ_STR(fan_reason_str(FAN_R_EMERGENCY), "emergency");
	T_EQ_STR(fan_reason_str(FAN_R_SILENT_HOT), "silent_overheat");
	T_EQ_STR(fan_reason_str(FAN_R_HOLD), "hold");
	T_EQ_STR(fan_reason_str(999), "?");

	printf("── 空指针不崩 ──\n");
	T_EQ_INT(fan_state_step(NULL, 50, 0), -1);
	fan_state_set_silent(NULL, 1);
	T_EQ_INT(fan_state_init(NULL, &cfg, 0), -1);

	T_SUMMARY();
}
