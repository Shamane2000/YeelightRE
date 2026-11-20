/*
 * mcufw_state.c -- internal lamp state model (phase 3.6).
 *
 * Single source of truth for what the lamp currently is.  Both network
 * front-ends (LAN JSON-RPC on 55443, miIO on 54321) mutate the same struct
 * through these setters; every setter clamps, bumps the mutation counter and
 * re-applies the state to the PWM backend.  That is what makes `toggle`
 * idempotent across the two channels: whoever wrote last wins, and the
 * hardware view always matches mcufw_state().
 */
#include "mcufw_pwm.h"
#include "mcufw_state.h"

static mcufw_state_t g_state;
static int g_state_ready;

static long clamp_l(long v, long lo, long hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

uint8_t mcufw_state_clamp_bright(long v)
{
	return (uint8_t)clamp_l(v, MCUFW_BRIGHT_MIN, MCUFW_BRIGHT_MAX);
}

uint16_t mcufw_state_clamp_ct(long v)
{
	return (uint16_t)clamp_l(v, MCUFW_CT_MIN, MCUFW_CT_MAX);
}

uint16_t mcufw_state_clamp_hue(long v)
{
	return (uint16_t)clamp_l(v, 0, MCUFW_HUE_MAX);
}

uint8_t mcufw_state_clamp_sat(long v)
{
	return (uint8_t)clamp_l(v, 0, MCUFW_SAT_MAX);
}

void mcufw_state_init(mcufw_state_t *st)
{
	if (!st)
		return;

	st->power = 0u;
	st->bright = MCUFW_BRIGHT_MAX;
	st->ct = 4000u;
	st->hue = 0u;
	st->sat = 0u;
	st->rgb_r = 0u;
	st->rgb_g = 0u;
	st->rgb_b = 0u;
	st->color_mode = MCUFW_COLOR_MODE_CT;
	st->cf_active = 0u;
	st->is_default = 0u;
	st->name[0] = '\0';
	st->mutations = 0u;

	mcufw_pwm_stats_reset();
	mcufw_state_apply(st);
}

mcufw_state_t *mcufw_state(void)
{
	if (!g_state_ready) {
		mcufw_state_init(&g_state);
		g_state_ready = 1;
	}
	return &g_state;
}

void mcufw_state_apply(mcufw_state_t *st)
{
	if (!st)
		return;

	if (!st->power) {
		/* bright 0 is the backend's "channel dark" command. */
		mcufw_pwm_set(0u, st->ct);
		return;
	}
	if (st->color_mode == MCUFW_COLOR_MODE_HSV)
		mcufw_pwm_hsv(st->hue, st->sat, st->bright);
	else if (st->color_mode == MCUFW_PWM_MODE_RGB)
		mcufw_pwm_rgb(st->rgb_r, st->rgb_g, st->rgb_b);
	else
		mcufw_pwm_set(st->bright, st->ct);
}

int mcufw_state_set_power(mcufw_state_t *st, int on)
{
	if (!st)
		return -1;

	st->power = on ? 1u : 0u;
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_toggle(mcufw_state_t *st)
{
	if (!st)
		return -1;

	st->power = st->power ? 0u : 1u;
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_set_bright(mcufw_state_t *st, long bright)
{
	if (!st)
		return -1;

	st->bright = mcufw_state_clamp_bright(bright);
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_set_ct(mcufw_state_t *st, long ct)
{
	if (!st)
		return -1;

	st->ct = mcufw_state_clamp_ct(ct);
	st->color_mode = MCUFW_COLOR_MODE_CT;
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_set_hsv(mcufw_state_t *st, long hue, long sat, long val)
{
	if (!st)
		return -1;

	st->hue = mcufw_state_clamp_hue(hue);
	st->sat = mcufw_state_clamp_sat(sat);
	st->bright = mcufw_state_clamp_bright(val);
	st->color_mode = MCUFW_COLOR_MODE_HSV;
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_set_rgb(mcufw_state_t *st, long r, long g, long b)
{
	if (!st)
		return -1;

	st->rgb_r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
	st->rgb_g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
	st->rgb_b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
	st->color_mode = MCUFW_PWM_MODE_RGB;
	st->mutations++;
	mcufw_state_apply(st);
	return 0;
}

int mcufw_state_set_name(mcufw_state_t *st, const char *name)
{
	size_t n = 0;

	if (!st)
		return -1;

	if (name) {
		while (name[n] && n < MCUFW_NAME_MAX) {
			/* Keep the name JSON-safe: it is echoed verbatim in
			 * miIO.info / get_prop responses. */
			if (name[n] != '"' && name[n] != '\\' &&
			    (unsigned char)name[n] >= 0x20u)
				st->name[n] = name[n];
			else
				st->name[n] = '_';
			n++;
		}
	}
	st->name[n] = '\0';
	st->mutations++;
	return 0;
}

int mcufw_state_set_cf(mcufw_state_t *st, int active)
{
	if (!st)
		return -1;

	st->cf_active = active ? 1u : 0u;
	st->mutations++;
	return 0;
}

int mcufw_state_set_default(mcufw_state_t *st, int is_default)
{
	if (!st)
		return -1;

	st->is_default = is_default ? 1u : 0u;
	st->mutations++;
	return 0;
}
