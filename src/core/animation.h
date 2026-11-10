#pragma once
#include <stdbool.h>
#include <stdint.h>

enum anim_easing {
	EASING_LINEAR = 0,
	EASING_EASE_IN_QUAD,
	EASING_EASE_OUT_QUAD,
	EASING_EASE_IN_OUT_QUAD,
	EASING_EASE_IN_CUBIC,
	EASING_EASE_OUT_CUBIC,
	EASING_EASE_IN_OUT_CUBIC,
	EASING_CUBIC_BEZIER,
};

struct anim_property {
	float current;
	float start;
	float target;
	int64_t start_us;
	int64_t delay_us;
	int64_t duration_us;
	enum anim_easing easing;
	float bezier[4];
	int repeat_count;    /* -1 = infinite, 0 = once */
	bool yoyo;           /* alternate forward/reverse */
	bool reversed;
	bool active;
};

float anim_ease(float t, enum anim_easing easing);
float anim_cubic_bezier(float t, float x1, float y1, float x2, float y2);
void anim_prop_init(struct anim_property *p, float initial_val);
void anim_prop_start(struct anim_property *p, float target, int64_t duration_ms,
		    enum anim_easing easing, int64_t now_us);
void anim_prop_start_ext(struct anim_property *p, float target, int64_t duration_ms,
			 int64_t delay_ms, enum anim_easing easing,
			 int repeat_count, bool yoyo, int64_t now_us);
float anim_prop_eval(struct anim_property *p, int64_t now_us);
uint32_t color_lerp(uint32_t c1, uint32_t c2, float t);
