/*
 * mcufw_pwm.h -- lamp PWM backend abstraction (phase 3.6).
 *
 * The state model (mcufw_state.c) is hardware-agnostic; this is the seam to
 * the light.  Phase 3.6 records the commanded values (mode/bright/ct/hue/sat/
 * val + the derived duty word) instead of programming GPT registers -- the
 * real register path (mcufw_lamp_set / GPT0 CMR0 per channel) lands in
 * phase 4.  Because nothing is written, the same code runs in the host test
 * and the assertions there verify exactly what the target would program.
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

typedef struct {
	uint32_t set_calls;  /* mcufw_pwm_set() invocations  */
	uint32_t hsv_calls;  /* mcufw_pwm_hsv() invocations  */
	uint8_t  mode;       /* last mode: 0 off, 2 ct, 3 hsv */
	uint8_t  bright;     /* last brightness 0..100       */
	uint16_t ct;         /* last colour temperature      */
	uint16_t hue;        /* last hue 0..359              */
	uint8_t  sat;        /* last saturation 0..100       */
	uint8_t  val;        /* last HSV value / brightness  */
	char     last_log[64]; /* human-readable last command */
} mcufw_pwm_stats_t;

/* Reset the recorded backend state. */
void mcufw_pwm_init(void);

/* White-channel command.  bright == 0 means "lamp off". */
void mcufw_pwm_set(uint8_t bright, uint16_t ct);

/* Colour-channel command. */
void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/* Read-only view of what the backend was last asked to do. */
const mcufw_pwm_stats_t *mcufw_pwm_stats(void);

void mcufw_pwm_stats_reset(void);

/* Duty word for a brightness percentage (0..MCUFW_PWM_PERIOD). */
uint16_t mcufw_pwm_duty(uint8_t bright);

#endif /* MCUFW_PWM_H */
