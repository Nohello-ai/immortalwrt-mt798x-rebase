#include "openfi_curve.h"

#include <string.h>

int curve_load(struct curve *c, const struct curve_point *pts, int n,
	       enum curve_mode mode, int min_y, int max_y, int emerg_x, int emerg_y)
{
	if (!c || !pts || n < 1 || n > CURVE_MAX_POINTS)
		return -1;
	memset(c, 0, sizeof(*c));
	memcpy(c->pt, pts, sizeof(c->pt[0]) * (size_t)n);
	c->n = n;
	c->mode = mode;
	c->min_y = min_y;
	c->max_y = max_y;
	c->emerg_x = emerg_x;
	c->emerg_y = emerg_y;
	return 0;
}

int curve_valid(const struct curve *c)
{
	if (!c || c->n < 2 || c->n > CURVE_MAX_POINTS)
		return 0;
	if (c->min_y > c->max_y)
		return 0;
	for (int i = 0; i < c->n; i++) {
		if (c->pt[i].y < c->min_y || c->pt[i].y > c->max_y)
			return 0;
		if (i && c->pt[i].x <= c->pt[i - 1].x)	/* x 必须严格递增 */
			return 0;
		if (i && c->pt[i].y < c->pt[i - 1].y)	/* y 单调不减 */
			return 0;
	}
	if (c->emerg_x && c->emerg_x <= c->pt[c->n - 1].x)
		return 0;
	return 1;
}

static int clamp(const struct curve *c, long v)
{
	if (v < c->min_y)
		return c->min_y;
	if (v > c->max_y)
		return c->max_y;
	return (int)v;
}

int curve_eval(const struct curve *c, int x)
{
	if (!c || c->n == 0)
		return 0;

	if (c->emerg_x && x >= c->emerg_x)
		return clamp(c, c->emerg_y);
	if (x <= c->pt[0].x)
		return clamp(c, c->pt[0].y);

	for (int i = 1; i < c->n; i++) {
		if (x <= c->pt[i].x) {
			if (c->mode == CURVE_LINEAR) {
				/* 在 pt[i-1] 与 pt[i] 之间插值 */
				long dx = c->pt[i].x - c->pt[i - 1].x;
				long dy = c->pt[i].y - c->pt[i - 1].y;

				if (dx <= 0)
					return clamp(c, c->pt[i].y);
				return clamp(c, c->pt[i - 1].y +
					     dy * (x - c->pt[i - 1].x) / dx);
			}
			return clamp(c, c->pt[i].y);
		}
	}
	return clamp(c, c->pt[c->n - 1].y);
}

void curve_hyst_init(struct curve_hyst *h, int min_delta, long interval_ms)
{
	if (!h)
		return;
	h->last_y = -1;
	h->last_ms = 0;
	h->min_delta = min_delta < 1 ? 1 : min_delta;
	h->interval_ms = interval_ms < 0 ? 0 : interval_ms;
}

int curve_hyst_should_apply(struct curve_hyst *h, int target_y, long long now_ms)
{
	if (!h)
		return 1;
	if (h->last_y < 0)
		return 1;			/* 第一次一定下发 */
	if (target_y > h->last_y)
		return target_y != h->last_y;	/* 上冲立刻响应 */
	if (h->last_y - target_y < h->min_delta)
		return 0;			/* 回落幅度不够 */
	if (h->interval_ms > 0 && now_ms - h->last_ms < h->interval_ms)
		return 0;			/* 距上次调整太近 */
	return 1;
}

void curve_hyst_applied(struct curve_hyst *h, int y, long long now_ms)
{
	if (!h)
		return;
	if (h->last_y != y) {
		h->last_y = y;
		h->last_ms = now_ms;
	}
}
