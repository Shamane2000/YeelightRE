/*
 * mcufw_pwm_hal.h -- GPT0 register + GPIO access seam (phase 4b.1).
 *
 * Link-time selection:
 *   host:   mcufw_pwm_hal_host.c   (word array + gpio stub)
 *   target: mcufw_pwm_hal_target.c (real MMIO + GPIO regs)
 */
#ifndef MCUFW_PWM_HAL_H
#define MCUFW_PWM_HAL_H

#include <stdint.h>

#define MCUFW_PWM_HAL_WORDS 256u
#define MCUFW_PWM_HAL_SPAN  (MCUFW_PWM_HAL_WORDS * 4u)

void     mcufw_pwm_hal_write(uint32_t offset, uint32_t value);
uint32_t mcufw_pwm_hal_read(uint32_t offset);
const char *mcufw_pwm_hal_name(void);

/* GPIO output control for LED enable pin (GPIO44) and future use.
 * pin: GPIO number (0..63). on: 0=low, 1=high. */
void mcufw_pwm_hal_gpio_set(uint8_t pin, uint8_t on);

/* ---- host backend only -------------------------------------------------- */
void     mcufw_pwm_hal_stub_reset(void);
uint32_t mcufw_pwm_hal_stub_read(uint32_t offset);
uint32_t mcufw_pwm_hal_stub_writes(void);
uint32_t mcufw_pwm_hal_stub_bad_offset(void);
uint32_t mcufw_pwm_hal_stub_latched_mask(void);
uint32_t mcufw_pwm_hal_stub_latch_errors(void);

/* Host-test GPIO readback: returns last value set for a pin. */
uint8_t  mcufw_pwm_hal_stub_gpio_get(uint8_t pin);

#endif /* MCUFW_PWM_HAL_H */
