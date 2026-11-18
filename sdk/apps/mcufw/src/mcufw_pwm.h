/*
 * mcufw_pwm.h -- lamp PWM backend abstraction (phase 3.6, register path 3.7).
 *
 * The state model (mcufw_state.c) is hardware-agnostic; this is the seam to
 * the light.  Since phase 3.7 the backend does not only record what it was
 * asked to do -- it programs the GPT0 channels through mcufw_pwm_hal_*():
 *
 *   white channels (CT / brightness)  -> CH0, CH1  (GPIO0/1, FSEL 1)
 *   colour channels (HSV value)       -> CH4, CH5  (GPIO4/5, FSEL 1)
 *
 * Register semantics proven in phase 3.3 (mcufw_gpt.c): edge-aligned PWM,
 * inverted polarity, period 0xC350, so the programmed word is
 * CMR0 = period - level (level 0 = dark, level 0xC350 = full on).
 *
 * The HAL backend is chosen at link time (real MMIO on the target, word array
 * in the host test), therefore the very same arithmetic is asserted by the
 * host test and executed on the lamp.
 */
#ifndef MCUFW_PWM_H
#define MCUFW_PWM_H

#include <stddef.h>
#include <stdint.h>

/* Same full-scale counter value as the reference app init table (0xC350). */
#define MCUFW_PWM_PERIOD 0xc350u

#define MCUFW_PWM_MODE_OFF 0u
#define MCUFW_PWM_MODE_CT  2u
#define MCUFW_PWM_MODE_HSV 3u

/* GPT0 channel indices of the four lamp strings (phase 3.3 init table:
 * pins 0/1/4/5, channels 0/1/4/5, all GPT0, FSEL 1). */
#define MCUFW_PWM_CH_WHITE0 0u
#define MCUFW_PWM_CH_WHITE1 1u
#define MCUFW_PWM_CH_COLOR0 4u
#define MCUFW_PWM_CH_COLOR1 5u

typedef struct {
	uint32_t set_calls;   /* mcufw_pwm_set() invocations  */
	uint32_t hsv_calls;   /* mcufw_pwm_hsv() invocations  */
	uint8_t  mode;        /* last mode: 0 off, 2 ct, 3 hsv */
	uint8_t  bright;      /* last brightness 0..100       */
	uint16_t ct;          /* last colour temperature      */
	uint16_t hue;         /* last hue 0..359              */
	uint8_t  sat;         /* last saturation 0..100       */
	uint8_t  val;         /* last HSV value / brightness  */
	/* --- phase 3.7: what was actually programmed ----------------------- */
	uint8_t  powered;     /* 1 = channels driven, 0 = all four dark */
	uint16_t white_level; /* level on CH0/CH1, 0..MCUFW_PWM_PERIOD */
	uint16_t color_level; /* level on CH4/CH5, 0..MCUFW_PWM_PERIOD */
	/* Last non-dark command, kept across a power off so it can be restored. */
	uint8_t  restore_mode;
	uint8_t  restore_bright;
	uint16_t restore_white;
	uint16_t restore_color;
	uint32_t reg_writes;  /* GPT0 register writes issued by this module */
	char     last_log[64]; /* human-readable last command */
} mcufw_pwm_stats_t;

/* Reset the recorded backend state (does not touch the hardware). */
void mcufw_pwm_init(void);

/* White-channel command.  bright == 0 means "lamp off" (all channels dark). */
void mcufw_pwm_set(uint8_t bright, uint16_t ct);

/* Colour-channel command.  val == 0 behaves like "off". */
void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/* Re-program the last non-dark command after a power off.  No-op if the lamp
 * was never driven since mcufw_pwm_init(). */
void mcufw_pwm_restore(void);

/* Read-only view of what the backend was last asked to do. */
const mcufw_pwm_stats_t *mcufw_pwm_stats(void);

void mcufw_pwm_stats_reset(void);

/* Duty word for a brightness percentage (0..MCUFW_PWM_PERIOD). */
uint16_t mcufw_pwm_duty(uint8_t bright);

/* Level last programmed per channel pair (0..MCUFW_PWM_PERIOD). */
uint16_t mcufw_pwm_white_level(void);
uint16_t mcufw_pwm_color_level(void);

#endif /* MCUFW_PWM_H */
