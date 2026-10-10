#include "openfi_fanctl.h"

#include <string.h>

void fan_cfg_default(struct fan_cfg *cfg, int min_speed)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->t1 = 55;
	cfg->t2 = 58;
	cfg->t3 = 61;
	cfg->t4 = 65;
	cfg->s1 = min_speed;
	cfg->s2 = min_speed + (100 - min_speed) / 3;
	cfg->s3 = min_speed + 2 * (100 - min_speed) / 3;
	cfg->s4 = 100;
	cfg->min_speed = min_speed;
	cfg->emergency_temp = 85;
}

int fan_cfg_valid(const struct fan_cfg *cfg)
{
	struct curve c;

	if (!cfg)
		return 0;
	if (cfg->min_speed < 0 || cfg->min_speed > 100)
		return 0;
	/* 复用通用模板的校验 —— 行为只有一份 */
	return fan_cfg_to_curve(cfg, &c) == 0 && curve_valid(&c);
}

int fan_cfg_to_curve(const struct fan_cfg *cfg, struct curve *out)
{
	struct curve_point pts[4];
	int maxy, miny;

	if (!cfg || !out)
		return -1;
	if (cfg->s1 < 0 || cfg->s4 > 100)
		return -1;
	if (cfg->t2 < cfg->t1 + 2 || cfg->t3 < cfg->t2 + 2 || cfg->t4 < cfg->t3 + 2)
		return -1;
	if (cfg->s2 < cfg->s1 || cfg->s3 < cfg->s2 || cfg->s4 < cfg->s3)
		return -1;
	if (cfg->emergency_temp <= cfg->t4)
		return -1;

	pts[0].x = cfg->t1; pts[0].y = cfg->s1;
	pts[1].x = cfg->t2; pts[1].y = cfg->s2;
	pts[2].x = cfg->t3; pts[2].y = cfg->s3;
	pts[3].x = cfg->t4; pts[3].y = cfg->s4;

	miny = cfg->min_speed;
	maxy = 100;
	if (miny > maxy)
		return -1;

	return curve_load(out, pts, 4, CURVE_STEP, miny, maxy,
			  cfg->emergency_temp, 100);
}

long fan_duty_for(int speed, long period_ns)
{
	if (speed < 0)
		speed = 0;
	if (speed > 100)
		speed = 100;
	/* 反极性:0 ns = 全速 */
	return period_ns * (100 - speed) / 100;
}

/* ================= 状态机 ================= */

const char *fan_reason_str(int r)
{
	switch (r) {
	case FAN_R_INIT:       return "init";
	case FAN_R_CURVE:      return "curve";
	case FAN_R_SILENT:     return "silent";
	case FAN_R_EMERGENCY:  return "emergency";
	case FAN_R_SILENT_HOT: return "silent_overheat";
	case FAN_R_MANUAL:     return "manual";
	case FAN_R_HOLD:       return "hold";
	}
	return "?";
}

int fan_state_init(struct fan_state *st, const struct fan_cfg *cfg, int silent_max_temp)
{
	if (!st || !cfg)
		return -1;
	memset(st, 0, sizeof(*st));
	if (!fan_cfg_valid(cfg))
		return -1;
	if (fan_cfg_to_curve(cfg, &st->curve) != 0)
		return -1;
	st->cfg = *cfg;
	/* 迟滞:降速要至少差 5 个点、且距上次调整至少 10 秒 ——
	   免得温度在拐点附近抖动时 PWM 一直变(风扇会有可听的啸叫)。 */
	curve_hyst_init(&st->hyst, 5, 10000);
	st->silent = 0;
	st->silent_max_temp = (silent_max_temp > 0) ? silent_max_temp : 75;
	st->last_applied = -1;
	st->last_reason = FAN_R_INIT;
	return 0;
}

void fan_state_set_silent(struct fan_state *st, int silent)
{
	if (!st)
		return;
	silent = silent ? 1 : 0;
	if (st->silent == silent)
		return;
	st->silent = silent;
	/*
	 * 【档位切换必须立刻生效】。
	 * 迟滞本来是为了挡住"温度在拐点附近抖动"造成的频繁调速,
	 * 但用户拨开关是一个明确的意图 —— 不能被"距上次调整不足 10 秒"挡回去,
	 * 否则拨回正常档后风扇还要再等最多 10 秒才恢复。
	 * 所以这里把迟滞状态重置掉,让下一步一定下发。
	 */
	curve_hyst_init(&st->hyst, st->hyst.min_delta, st->hyst.interval_ms);
}

int fan_state_step(struct fan_state *st, int unified_temp, long long now_ms)
{
	int target, reason;

	if (!st)
		return -1;

	if (unified_temp == TEMP_INVALID) {
		/* 温度读不到:不要乱改转速(可能是采样进程还没起来)。
		   首次则给一个安全的中速,避免完全不转。 */
		if (st->last_applied < 0) {
			target = 50;
			reason = FAN_R_INIT;
			goto apply;
		}
		st->last_reason = FAN_R_HOLD;
		return -1;
	}

	/* 安全第一:不管静默不静默,过热都满转 */
	if (unified_temp >= st->cfg.emergency_temp) {
		target = 100;
		reason = FAN_R_EMERGENCY;
		goto force;
	}

	if (st->silent) {
		if (unified_temp >= st->silent_max_temp) {
			/* 静默中过热:强制开风扇,但只按曲线给,不拉满 */
			target = curve_eval(&st->curve, unified_temp);
			reason = FAN_R_SILENT_HOT;
			goto force;
		}
		target = 0;
		reason = FAN_R_SILENT;
		goto force;
	}

	/*
	 * 手动固定转速。
	 * ★ 放在紧急温度/静默兜底【之后】—— 那两个是安全兜底,
	 *   不能被"用户固定低速"越过(否则界面一个手滑就能把过热保护关掉)。
	 */
	if (st->cfg.mode == FAN_MODE_MANUAL && st->cfg.manual_speed >= 0) {
		int v = st->cfg.manual_speed;

		if (v < st->cfg.min_speed)
			v = st->cfg.min_speed;
		if (v > 100)
			v = 100;
		target = v;
		reason = FAN_R_MANUAL;
		goto force;
	}

	target = curve_eval(&st->curve, unified_temp);
	reason = FAN_R_CURVE;
	if (!curve_hyst_should_apply(&st->hyst, target, now_ms)) {
		st->last_reason = FAN_R_HOLD;
		return -1;
	}

apply:
	curve_hyst_applied(&st->hyst, target, now_ms);
	st->last_applied = target;
	st->last_reason = reason;
	return target;

force:
	/* 静默/紧急这类切换必须立刻生效,不受迟滞限制 */
	curve_hyst_applied(&st->hyst, target, now_ms);
	st->last_applied = target;
	st->last_reason = reason;
	return target;
}
