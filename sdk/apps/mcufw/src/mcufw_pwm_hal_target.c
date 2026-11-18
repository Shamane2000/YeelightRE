/* mcufw_pwm_hal_target.c -- GPT0 register seam, target backend (phase 3.7).
 *
 * Every lamp write in the firmware goes through mcufw_pwm_hal_write(); this is
 * the single place that touches the real peripheral window.  Using the same
 * offsets as the proven 3.3 driver (GPT0_BASE 0x46070000, channel block
 * 0x200 + ch*0x30, CMR0 +0x10, USER_REQ 0x0f0) means the host test and the
 * target execute identical arithmetic.
 */
#include "mcufw_regs.h"
#include "mcufw_pwm_hal.h"

void mcufw_pwm_hal_write(uint32_t offset, uint32_t value)
{
	REG32(GPT0_BASE + offset) = value;
}

uint32_t mcufw_pwm_hal_read(uint32_t offset)
{
	return REG32(GPT0_BASE + offset);
}

const char *mcufw_pwm_hal_name(void)
{
	return "gpt0-mmio";
}
