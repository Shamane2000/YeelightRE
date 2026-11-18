/*
 * mcufw_pwm_hal.h -- GPT0 register access seam (phase 3.7).
 *
 * One named HAL layer sits between "what the lamp should do" (mcufw_pwm.c,
 * mcufw_gpt.c) and "where the GPT0 registers are".  Two implementations of
 * the same three entry points exist and are selected at LINK time by the
 * Makefile -- no #ifdef in any caller:
 *
 *   src/mcufw_pwm_hal_target.c  *((volatile uint32_t *)(GPT0_BASE + off))
 *                               -> real peripheral window (firmware build)
 *   src/mcufw_pwm_hal_host.c    word array modelling the GPT0 window
 *                               -> host test asserts exact offsets/values
 *                                  without any hardware access
 *
 * The host backend additionally exposes its window for assertions; the target
 * backend deliberately does NOT define those symbols, so no code that ends up
 * in the firmware image can accidentally depend on the stub.
 */
#ifndef MCUFW_PWM_HAL_H
#define MCUFW_PWM_HAL_H

#include <stdint.h>

/* Window covered by the mocked backend: 256 words = 1 KiB.  That spans the
 * GPT0 header (CNT_EN 0x000 .. USER_REQ 0x0f0) plus all six channel blocks,
 * the last of which (CH5) ends at 0x200 + 5*0x30 + 0x20 = 0x310. */
#define MCUFW_PWM_HAL_WORDS 256u
#define MCUFW_PWM_HAL_SPAN  (MCUFW_PWM_HAL_WORDS * 4u)

/* Write/read one 32-bit GPT0 register, offset relative to GPT0_BASE. */
void     mcufw_pwm_hal_write(uint32_t offset, uint32_t value);
uint32_t mcufw_pwm_hal_read(uint32_t offset);

/* Backend identity for the UART banner and the host-test sanity check. */
const char *mcufw_pwm_hal_name(void);

/* ---- host backend only (src/mcufw_pwm_hal_host.c) ------------------------
 * mcufw_pwm_hal_host.c is the only definition of these; they let the host
 * test read back what the driver programmed. */
void     mcufw_pwm_hal_stub_reset(void);
uint32_t mcufw_pwm_hal_stub_read(uint32_t offset);
uint32_t mcufw_pwm_hal_stub_writes(void);
/* Number of accesses outside the GPT0 window (must stay 0 -> proves the
 * driver never writes past the peripheral). */
uint32_t mcufw_pwm_hal_stub_bad_offset(void);
/* Bitmask of channels whose CMR0 write was followed by the matching
 * CHn_CMR_UPDT request in USER_REQ, plus the count of mismatching latches. */
uint32_t mcufw_pwm_hal_stub_latched_mask(void);
uint32_t mcufw_pwm_hal_stub_latch_errors(void);

#endif /* MCUFW_PWM_HAL_H */
