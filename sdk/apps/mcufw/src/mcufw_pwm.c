/*
 * mcufw_pwm.c -- lamp PWM backend abstraction (phase 3.6).
 *
 * The state model calls exactly two entry points:
 *
 *   mcufw_pwm_set(bright, ct)     -- white channel, bright 0 == off
 *   mcufw_pwm_hsv(hue, sat, val)  -- colour channel
 *
 * Phase 3.6 delivers the abstraction layer only: no GPT register is written
 * here.  On the target the real hook-up (mcufw_lamp_set() from mcufw_gpt.c,
 * plus the per-channel CMR0 programming of the colour channels) is phase 4
 * work; both functions therefore only record what they were asked to do.
 *
 * Recording instead of writing keeps the module host-testable and lets the
 * target UART banner dump the last commanded PWM word without a debugger.
 */
#include "mcufw_pwm.h"

static mcufw_pwm_stats_t g_pwm;

/* --------------------------------------------------------------- logging -- */
static void log_put(char *dst, size_t cap, size_t *pos, const char *s)
{
	while (*s && *pos + 1u < cap)
		dst[(*pos)++] = *s++;
	dst[*pos] = '\0';
}

static void log_num(char *dst, size_t cap, size_t *pos, long v)
{
	char tmp[12];
	int n = 0;
	unsigned long u;

	if (v < 0) {
		log_put(dst, cap, pos, "-");
		u = (unsigned long)(-(v + 1)) + 1ul;
	} else {
		u = (unsigned long)v;
	}
	do {
		tmp[n++] = (char)('0' + (int)(u % 10ul));
		u /= 10ul;
	} while (u && n < (int)sizeof(tmp));
	while (n > 0 && *pos + 1u < cap)
		dst[(*pos)++] = tmp[--n];
	dst[*pos] = '\0';
}

/* ------------------------------------------------------------- backend ---- */
void mcufw_pwm_init(void)
{
	g_pwm.set_calls = 0;
	g_pwm.hsv_calls = 0;
	g_pwm.mode = MCUFW_PWM_MODE_OFF;
	g_pwm.bright = 0;
	g_pwm.ct = 0;
	g_pwm.hue = 0;
	g_pwm.sat = 0;
	g_pwm.val = 0;
	g_pwm.last_log[0] = '\0';
}

void mcufw_pwm_set(uint8_t bright, uint16_t ct)
{
	size_t pos = 0;

	g_pwm.set_calls++;
	if (bright == 0u) {
		g_pwm.mode = MCUFW_PWM_MODE_OFF;
		g_pwm.bright = 0;
		g_pwm.ct = ct;
		g_pwm.last_log[0] = '\0';
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm off ct=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)ct);
		return;
	}

	g_pwm.mode = MCUFW_PWM_MODE_CT;
	g_pwm.bright = bright;
	g_pwm.ct = ct;
	g_pwm.last_log[0] = '\0';
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm ct bright=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)bright);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " ct=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)ct);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " duty=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
		(long)mcufw_pwm_duty(bright));
}

void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
	size_t pos = 0;

	g_pwm.hsv_calls++;
	g_pwm.mode = (val == 0u) ? MCUFW_PWM_MODE_OFF : MCUFW_PWM_MODE_HSV;
	g_pwm.hue = hue;
	g_pwm.sat = sat;
	g_pwm.val = val;
	g_pwm.bright = val;
	g_pwm.last_log[0] = '\0';
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm hsv h=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)hue);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " s=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)sat);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " v=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)val);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " duty=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
		(long)mcufw_pwm_duty(val));
}

const mcufw_pwm_stats_t *mcufw_pwm_stats(void)
{
	return &g_pwm;
}

void mcufw_pwm_stats_reset(void)
{
	mcufw_pwm_init();
}

uint16_t mcufw_pwm_duty(uint8_t bright)
{
	/* Linear map 0..100 % -> 0..MCUFW_PWM_PERIOD counts.  The reference
	 * firmware uses the same full-scale counter value (0xC350) for every
	 * lamp channel, so the duty word is directly comparable. */
	if (bright == 0u)
		return 0u;
	if (bright >= 100u)
		return MCUFW_PWM_PERIOD;
	return (uint16_t)(((uint32_t)bright * MCUFW_PWM_PERIOD) / 100u);
}
