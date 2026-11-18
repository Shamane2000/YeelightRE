/*
 * mcufw_pwm.h -- color2 (YLDP06YL) 4-Kanal-RGBW-PWM + GPIO44 Enable (Phase 4b.1).
 *
 * Hardware-Fakten aus color2_142_0027.elf (Ghidra):
 *   - GPT0 CH0 -> GPIO0, CH1 -> GPIO1, CH4 -> GPIO4, CH5 -> GPIO5
 *   - Periode 50000 (0xC350), inverted: CMR0 = period - duty
 *   - GPIO44 (0x2c) = active-high LED-Power-Enable (0=aus, 1=an)
 *
 * Kanal-Zuordnung (color2 RGBW):
 *   CH0 = Rot, CH1 = Grün, CH4 = Blau, CH5 = Weiß
 *
 * CT-Mixing: warmweiß/kaltweiß wird über RGB+Anteil W gemischt.
 * set_rgb(h,s,v) steuert alle 4 Kanäle getrennt.
 */
#ifndef MCUFW_PWM_H
#define MCUFW_PWM_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_PWM_PERIOD 0xc350u

#define MCUFW_PWM_MODE_OFF 0u
#define MCUFW_PWM_MODE_CT  2u
#define MCUFW_PWM_MODE_HSV 3u

/* GPT0 channel indices for color2 RGBW strings. */
#define MCUFW_PWM_CH_R 0u  /* GPIO0 */
#define MCUFW_PWM_CH_G 1u  /* GPIO1 */
#define MCUFW_PWM_CH_B 4u  /* GPIO4 */
#define MCUFW_PWM_CH_W 5u  /* GPIO5 */

/* GPIO44 = LED power enable pin. */
#define MCUFW_PIN_LED_ENABLE 44u

typedef struct {
	uint32_t set_calls;
	uint32_t hsv_calls;
	uint8_t  mode;        /* 0 off, 2 ct, 3 hsv */
	uint8_t  bright;      /* 0..100 */
	uint16_t ct;          /* colour temperature */
	uint16_t hue;         /* 0..359 */
	uint8_t  sat;         /* 0..100 */
	uint8_t  val;         /* HSV value */
	uint8_t  powered;     /* 1 = channels driven, 0 = dark */
	uint16_t ch_level[4]; /* R/G/B/W levels 0..MCUFW_PWM_PERIOD */
	uint8_t  led_enable;  /* GPIO44 state: 0=off, 1=on */
	uint8_t  restore_mode;
	uint8_t  restore_bright;
	uint16_t restore_ch[4];
	uint32_t reg_writes;
	uint8_t  dirty;
	uint32_t service_applied;
	uint32_t service_idle;
	uint32_t skipped_identical;
	char     last_log[64];
} mcufw_pwm_stats_t;

void mcufw_pwm_init(void);
void mcufw_pwm_set(uint8_t bright, uint16_t ct);
void mcufw_pwm_hsv(uint16_t hue, uint8_t sat, uint8_t val);
void mcufw_pwm_restore(void);
int  mcufw_pwm_service(void);
void mcufw_pwm_invalidate(void);
uint32_t mcufw_pwm_cmr0(uint8_t ch);
const mcufw_pwm_stats_t *mcufw_pwm_stats(void);
void mcufw_pwm_stats_reset(void);
uint16_t mcufw_pwm_duty(uint8_t bright);
uint16_t mcufw_pwm_channel_level(uint8_t ch_idx); /* ch_idx 0..3 -> R/G/B/W */

#endif /* MCUFW_PWM_H */
