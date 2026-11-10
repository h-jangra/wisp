#include "core/animation.h"
#include <math.h>

static inline float sample_curve(float a, float b, float c, float t)
{
	return ((a * t + b) * t + c) * t;
}

static inline float sample_curve_derivative(float a, float b, float c, float t)
{
	return (3.0f * a * t + 2.0f * b) * t + c;
}

float anim_cubic_bezier(float t, float x1, float y1, float x2, float y2)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;

	float cx = 3.0f * x1;
	float bx = 3.0f * (x2 - x1) - cx;
	float ax = 1.0f - cx - bx;

	float cy = 3.0f * y1;
	float by = 3.0f * (y2 - y1) - cy;
	float ay = 1.0f - cy - by;

	/* Solve for u such that sample_curve(ax, bx, cx, u) == t using Newton's method */
	float u = t;
	for (int i = 0; i < 8; i++) {
		float x_est = sample_curve(ax, bx, cx, u) - t;
		if (fabsf(x_est) < 1e-4f)
			break;
		float d = sample_curve_derivative(ax, bx, cx, u);
		if (fabsf(d) < 1e-5f)
			break;
		u -= x_est / d;
	}

	/* Clamp u in [0, 1] */
	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;

	return sample_curve(ay, by, cy, u);
}

float anim_ease(float t, enum anim_easing easing)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;

	switch (easing) {
	case EASING_LINEAR:
		return t;
	case EASING_EASE_IN_QUAD:
		return t * t;
	case EASING_EASE_OUT_QUAD:
		return t * (2.0f - t);
	case EASING_EASE_IN_OUT_QUAD:
		return (t < 0.5f) ? (2.0f * t * t) : (-1.0f + (4.0f - 2.0f * t) * t);
	case EASING_EASE_IN_CUBIC:
		return t * t * t;
	case EASING_EASE_OUT_CUBIC: {
		float f = t - 1.0f;
		return f * f * f + 1.0f;
	}
	case EASING_EASE_IN_OUT_CUBIC:
		return (t < 0.5f) ? (4.0f * t * t * t)
				  : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
	default:
		return t;
	}
}

void anim_prop_init(struct anim_property *p, float initial_val)
{
	p->current = initial_val;
	p->start = initial_val;
	p->target = initial_val;
	p->start_us = 0;
	p->delay_us = 0;
	p->duration_us = 0;
	p->easing = EASING_LINEAR;
	p->repeat_count = 0;
	p->yoyo = false;
	p->reversed = false;
	p->active = false;
	p->bezier[0] = 0.25f;
	p->bezier[1] = 0.1f;
	p->bezier[2] = 0.25f;
	p->bezier[3] = 1.0f;
}

void anim_prop_start(struct anim_property *p, float target, int64_t duration_ms,
		    enum anim_easing easing, int64_t now_us)
{
	anim_prop_start_ext(p, target, duration_ms, 0, easing, 0, false, now_us);
}

void anim_prop_start_ext(struct anim_property *p, float target, int64_t duration_ms,
			 int64_t delay_ms, enum anim_easing easing,
			 int repeat_count, bool yoyo, int64_t now_us)
{
	p->start = p->current;
	p->target = target;
	p->start_us = now_us;
	p->delay_us = delay_ms * 1000;
	p->duration_us = duration_ms * 1000;
	p->easing = easing;
	p->repeat_count = repeat_count;
	p->yoyo = yoyo;
	p->reversed = false;
	p->active = (duration_ms > 0 && p->start != target);
	if (!p->active)
		p->current = target;
}

float anim_prop_eval(struct anim_property *p, int64_t now_us)
{
	if (!p->active)
		return p->current;

	if (now_us <= p->start_us + p->delay_us)
		return p->start;

	int64_t elapsed = now_us - (p->start_us + p->delay_us);
	if (elapsed >= p->duration_us) {
		if (p->repeat_count != 0) {
			if (p->repeat_count > 0)
				p->repeat_count--;
			p->start_us = now_us;
			p->delay_us = 0;
			if (p->yoyo) {
				float tmp = p->start;
				p->start = p->target;
				p->target = tmp;
				p->reversed = !p->reversed;
			}
			return p->start;
		}

		p->current = p->target;
		p->active = false;
		return p->current;
	}

	float t = (float)elapsed / (float)p->duration_us;
	float eased;
	if (p->easing == EASING_CUBIC_BEZIER)
		eased = anim_cubic_bezier(t, p->bezier[0], p->bezier[1], p->bezier[2], p->bezier[3]);
	else
		eased = anim_ease(t, p->easing);

	p->current = p->start + (p->target - p->start) * eased;
	return p->current;
}

uint32_t color_lerp(uint32_t c1, uint32_t c2, float t)
{
	if (t <= 0.0f) return c1;
	if (t >= 1.0f) return c2;
	int a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
	int a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
	int a = (int)(a1 + (a2 - a1) * t);
	int r = (int)(r1 + (r2 - r1) * t);
	int g = (int)(g1 + (g2 - g1) * t);
	int b = (int)(b1 + (b2 - b1) * t);
	return ((uint32_t)(a & 0xFF) << 24) |
	       ((uint32_t)(r & 0xFF) << 16) |
	       ((uint32_t)(g & 0xFF) << 8) |
	       ((uint32_t)(b & 0xFF));
}
