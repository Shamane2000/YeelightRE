/*
 * mcufw_pwm.c -- lamp PWM backend: state -> GPT0 register writes (phase 3.7).
 *
 * The state model calls exactly two entry points:
 *
 *   mcufw_pwm_set(bright, ct)     -- white channel, bright 0 == off
 *   mcufw_pwm_hsv(hue, sat, val)  -- colour channel
 *
 * Phase 3.6 only recorded the command.  Phase 3.7 wires the recorded command
 * to the GPT0 channels of phase 3.3, through the mcufw_pwm_hal_* seam:
 *
 *   white pair  CH0/CH1  <- brightness / colour temperature
 *   colour pair CH4/CH5  <- HSV value
 *
 * Register arithmetic is the one proven in mcufw_gpt.c: edge-aligned PWM with
 * inverted polarity and period 0xC350, so
 *
 *   CMR0 = MCUFW_PWM_PERIOD - level
 *
 *   level 0x0000 (dark)        -> CMR0 0xC350
 *   level 0x61A8 (50 %)        -> CMR0 0x61A8
 *   level 0xC350 (100 %)       -> CMR0 0x0000
 *
 * Every CMR0 write is followed by the channel's CMR-update request in
 * USER_REQ, exactly like the 3.3 driver, so the new match value latches on the
 * next counter period instead of tearing mid-period.
 *
 * What is PROVEN vs ASSUMED
 *   proven : channel->pin map (0/1/4/5), GPT0 base, channel block 0x200+ch*0x30,
 *            CMR0 +0x10, USER_REQ latch, period 0xC350, inverted polarity.
 *   assumed: that the four strings split into a white pair (CH0/CH1) and a
 *            colour pair (CH4/CH5).  The 3.3 driver writes the same level to
 *            all four channels, so this split is the 3.7 mapping hypothesis; it
 *            needs silicon confirmation (lamp on .115, phase later) before it
 *            counts as proven.  Same for a real hue/saturation -> channel mix:
 *            no per-hue channel table is present in the reference image, so
 *            only the HSV V component drives the duty word; hue/sat are
 *            recorded for the future mix table.
 *
 * OWNERSHIP (phase 3.8) -- READ BEFORE ADDING A CALL
 *   This module is the SINGLE WRITER on the four lamp channels of GPT0.
 *   mcufw_lamp_set() (mcufw_gpt.c) may only be called from the bounded boot
 *   diagnosis in main.c; the runtime loop never programs a channel itself, it
 *   only calls mcufw_pwm_service().  Two properties make that hold:
 *
 *     - mcufw_pwm_service() issues no register write at all; it just reports
 *       and clears the dirty flag, so a tick without a state change performs
 *       exactly 0 GPT0 writes (no periodic re-writing of CMR0).
 *     - a command that equals the currently programmed command is skipped
 *       (skipped_identical++), so repeating get_prop/brightness-equal requests
 *       cannot rewrite CMR0 either.
 *
 *   The Makefile target `check-owner` enforces the single call site
 *   mechanically (grep guard); host_test.c section `runtime_owner` asserts the
 *   resulting register behaviour.
 */
#include "mcufw_regs.h"
#include "mcufw_pwm.h"
#include "mcufw_pwm_hal.h"

static mcufw_pwm_stats_t g_pwm;

/* The command that is currently programmed into the four channels.  Used to
 * skip redundant commands (a repeated identical set_bright must not rewrite
 * CMR0) and to prove the idle invariant of the runtime loop. */
typedef struct {
	uint8_t  mode;
	uint8_t  bright;
	uint16_t ct;
	uint16_t level;
} pwm_programmed_t;

static pwm_programmed_t g_prog;

/* Sentinel for "the registers hold nothing we know about" (e.g. after the
 * boot ramp wrote CMR0 behind this module's back).  Any first command then
 * programs unconditionally instead of being skipped as identical. */
#define PWM_PROG_UNKNOWN 0xFFu

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

static void log_reset(void)
{
	g_pwm.last_log[0] = '\0';
}

/* ---------------------------------------------------- GPT0 channel writes -- */
/* Program one channel: inverted duty into CMR0, then request the latch. */
static void channel_level(uint8_t ch, uint16_t level)
{
	uint32_t cmr;

	if (level > MCUFW_PWM_PERIOD)
		level = MCUFW_PWM_PERIOD;

	/* Low-active string (3.3): level 0 -> CMR0 = period (dark). */
	cmr = (uint32_t)MCUFW_PWM_PERIOD - (uint32_t)level;
	mcufw_pwm_hal_write(GPT_CHAN(ch) + GPT_CH_CMR0, cmr);
	mcufw_pwm_hal_write(GPT_USER_REQ, GPT_CMR_UPDT(ch));
	g_pwm.reg_writes += 2u;
}

static void pair_level(uint8_t a, uint8_t b, uint16_t level)
{
	channel_level(a, level);
	channel_level(b, level);
}

/* Which channel pair the current mode drives. */
typedef enum {
	DRIVE_OFF = 0,
	DRIVE_WHITE,
	DRIVE_COLOR
} drive_t;

/* mode-specific channel programming: one pair driven, the other dark. */
static void drive(drive_t what, uint16_t level)
{
	switch (what) {
	case DRIVE_WHITE:
		pair_level(MCUFW_PWM_CH_WHITE0, MCUFW_PWM_CH_WHITE1, level);
		pair_level(MCUFW_PWM_CH_COLOR0, MCUFW_PWM_CH_COLOR1, 0u);
		g_pwm.white_level = level;
		g_pwm.color_level = 0u;
		break;
	case DRIVE_COLOR:
		pair_level(MCUFW_PWM_CH_WHITE0, MCUFW_PWM_CH_WHITE1, 0u);
		pair_level(MCUFW_PWM_CH_COLOR0, MCUFW_PWM_CH_COLOR1, level);
		g_pwm.white_level = 0u;
		g_pwm.color_level = level;
		break;
	case DRIVE_OFF:
	default:
		pair_level(MCUFW_PWM_CH_WHITE0, MCUFW_PWM_CH_WHITE1, 0u);
		pair_level(MCUFW_PWM_CH_COLOR0, MCUFW_PWM_CH_COLOR1, 0u);
		g_pwm.white_level = 0u;
		g_pwm.color_level = 0u;
		break;
	}
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
	g_pwm.powered = 0;
	g_pwm.white_level = 0;
	g_pwm.color_level = 0;
	g_pwm.restore_mode = MCUFW_PWM_MODE_OFF;
	g_pwm.restore_bright = 0;
	g_pwm.restore_white = 0;
	g_pwm.restore_color = 0;
	g_pwm.reg_writes = 0;
	g_pwm.dirty = 0;
	g_pwm.service_applied = 0;
	g_pwm.service_idle = 0;
	g_pwm.skipped_identical = 0;
	g_pwm.last_log[0] = '\0';

	g_prog.mode = PWM_PROG_UNKNOWN;
	g_prog.bright = 0u;
	g_prog.ct = 0u;
	g_prog.level = 0u;
}

void mcufw_pwm_invalidate(void)
{
	/* Someone else touched the channels (boot ramp diagnosis).  Forget the
	 * programmed command so the next state command writes again. */
	g_prog.mode = PWM_PROG_UNKNOWN;
}

void mcufw_pwm_set(uint8_t bright, uint16_t ct)
{
	size_t pos = 0;
	uint16_t level;

	g_pwm.set_calls++;
	log_reset();
	g_pwm.ct = ct;

	if (bright == 0u) {
		if (g_prog.mode == MCUFW_PWM_MODE_OFF) {
			/* Channels already dark: keep the registers untouched. */
			g_pwm.mode = MCUFW_PWM_MODE_OFF;
			g_pwm.bright = 0;
			g_pwm.powered = 0;
			g_pwm.skipped_identical++;
			log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
				"pwm off ct=");
			log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
				(long)ct);
			log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
				" same");
			return;
		}
		/* Power-off: all four channels to level 0 (CMR0 = period).
		 * The last non-dark command stays in restore_* so a power-on
		 * can bring the lamp back where it was. */
		g_pwm.mode = MCUFW_PWM_MODE_OFF;
		g_pwm.bright = 0;
		g_pwm.powered = 0;
		drive(DRIVE_OFF, 0u);
		g_prog.mode = MCUFW_PWM_MODE_OFF;
		g_prog.bright = 0u;
		g_prog.ct = ct;
		g_prog.level = 0u;
		g_pwm.dirty = 1;
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm off ct=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)ct);
		return;
	}

	level = mcufw_pwm_duty(bright);
	if (g_prog.mode == MCUFW_PWM_MODE_CT && g_prog.bright == bright &&
	    g_prog.ct == ct && g_prog.level == level) {
		/* Repeated identical command: do not rewrite CMR0. */
		g_pwm.mode = MCUFW_PWM_MODE_CT;
		g_pwm.bright = bright;
		g_pwm.powered = 1;
		g_pwm.skipped_identical++;
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
			"pwm ct bright=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)bright);
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " same");
		return;
	}

	g_pwm.mode = MCUFW_PWM_MODE_CT;
	g_pwm.bright = bright;
	g_pwm.powered = 1;
	drive(DRIVE_WHITE, level);
	g_prog.mode = MCUFW_PWM_MODE_CT;
	g_prog.bright = bright;
	g_prog.ct = ct;
	g_prog.level = level;
	g_pwm.dirty = 1;
	g_pwm.restore_mode = MCUFW_PWM_MODE_CT;
	g_pwm.restore_bright = bright;
	g_pwm.restore_white = level;
	g_pwm.restore_color = 0u;

	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm ct bright=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)bright);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " ct=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)ct);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " duty=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)level);
}

void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
	size_t pos = 0;
	uint16_t level;

	g_pwm.hsv_calls++;
	log_reset();
	g_pwm.hue = hue;
	g_pwm.sat = sat;
	g_pwm.val = val;

	if (val == 0u) {
		if (g_prog.mode == MCUFW_PWM_MODE_OFF) {
			g_pwm.mode = MCUFW_PWM_MODE_OFF;
			g_pwm.bright = 0;
			g_pwm.powered = 0;
			g_pwm.skipped_identical++;
			log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
				"pwm hsv v=0 same");
			return;
		}
		g_pwm.mode = MCUFW_PWM_MODE_OFF;
		g_pwm.bright = 0;
		g_pwm.powered = 0;
		drive(DRIVE_OFF, 0u);
		g_prog.mode = MCUFW_PWM_MODE_OFF;
		g_prog.bright = 0u;
		g_prog.ct = 0u;
		g_prog.level = 0u;
		g_pwm.dirty = 1;
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm hsv h=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)hue);
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " s=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)sat);
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " v=0 duty=0");
		return;
	}

	level = mcufw_pwm_duty(val);
	if (g_prog.mode == MCUFW_PWM_MODE_HSV && g_prog.bright == val &&
	    g_prog.level == level) {
		g_pwm.mode = MCUFW_PWM_MODE_HSV;
		g_pwm.bright = val;
		g_pwm.powered = 1;
		g_pwm.skipped_identical++;
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos,
			"pwm hsv v=");
		log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)val);
		log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " same");
		return;
	}

	g_pwm.mode = MCUFW_PWM_MODE_HSV;
	g_pwm.bright = val;
	g_pwm.powered = 1;
	drive(DRIVE_COLOR, level);
	g_prog.mode = MCUFW_PWM_MODE_HSV;
	g_prog.bright = val;
	g_prog.ct = 0u;
	g_prog.level = level;
	g_pwm.dirty = 1;
	g_pwm.restore_mode = MCUFW_PWM_MODE_HSV;
	g_pwm.restore_bright = val;
	g_pwm.restore_white = 0u;
	g_pwm.restore_color = level;

	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, "pwm hsv h=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)hue);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " s=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)sat);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " v=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)val);
	log_put(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, " duty=");
	log_num(g_pwm.last_log, sizeof(g_pwm.last_log), &pos, (long)level);
}

void mcufw_pwm_restore(void)
{
	if (g_pwm.restore_white == 0u && g_pwm.restore_color == 0u)
		return;

	if (g_pwm.restore_mode == MCUFW_PWM_MODE_HSV) {
		g_pwm.mode = MCUFW_PWM_MODE_HSV;
		g_pwm.bright = g_pwm.restore_bright;
		drive(DRIVE_COLOR, g_pwm.restore_color);
		g_prog.mode = MCUFW_PWM_MODE_HSV;
		g_prog.bright = g_pwm.restore_bright;
		g_prog.ct = 0u;
		g_prog.level = g_pwm.restore_color;
	} else {
		g_pwm.mode = MCUFW_PWM_MODE_CT;
		g_pwm.bright = g_pwm.restore_bright;
		drive(DRIVE_WHITE, g_pwm.restore_white);
		g_prog.mode = MCUFW_PWM_MODE_CT;
		g_prog.bright = g_pwm.restore_bright;
		g_prog.ct = g_pwm.ct;
		g_prog.level = g_pwm.restore_white;
	}
	g_pwm.powered = 1;
	g_pwm.dirty = 1;
}

int mcufw_pwm_service(void)
{
	/* Called once per runtime tick.  Deliberately performs NO register
	 * write: the state path already programmed the channels, this call only
	 * consumes the dirty flag so the loop can report "state was applied"
	 * versus "nothing to do" without ever touching CMR0 itself. */
	if (!g_pwm.dirty) {
		g_pwm.service_idle++;
		return 0;
	}
	g_pwm.dirty = 0;
	g_pwm.service_applied++;
	return 1;
}

uint32_t mcufw_pwm_cmr0(uint8_t ch)
{
	return mcufw_pwm_hal_read(GPT_CHAN(ch) + GPT_CH_CMR0);
}

const mcufw_pwm_stats_t *mcufw_pwm_stats(void)
{
	return &g_pwm;
}

void mcufw_pwm_stats_reset(void)
{
	mcufw_pwm_init();
}

uint16_t mcufw_pwm_white_level(void)
{
	return g_pwm.white_level;
}

uint16_t mcufw_pwm_color_level(void)
{
	return g_pwm.color_level;
}

uint16_t mcufw_pwm_duty(uint8_t bright)
{
	/* Linear map 0..100 % -> 0..MCUFW_PWM_PERIOD counts.  The reference
	 * firmware uses the same full-scale counter value (0xC350) for every
	 * lamp channel, so the duty word is directly comparable.  Because
	 * 0xC350 / 100 = 500 is an integer, every whole percentage lands on an
	 * exact count (1 % -> 0x01F4, 50 % -> 0x61A8); no rounding is needed
	 * and none is applied. */
	if (bright == 0u)
		return 0u;
	if (bright >= 100u)
		return MCUFW_PWM_PERIOD;
	return (uint16_t)(((uint32_t)bright * MCUFW_PWM_PERIOD) / 100u);
}
