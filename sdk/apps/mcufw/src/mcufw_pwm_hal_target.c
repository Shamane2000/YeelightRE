/* mcufw_pwm_hal_target.c -- GPT0 + GPIO seam, target backend (phase 4b.1). */
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

const char *mcufw_pwm_hal_name(void) { return "gpt0-mmio"; }

void mcufw_pwm_hal_gpio_set(uint8_t pin, uint8_t on)
{
	/* GPIO bank = pin >> 5, bit = 1 << (pin & 31).
	 * GPSR = set, GPCR = clear. Base from mcufw_regs.h. */
	uint32_t bank = (uint32_t)(pin >> 5);
	uint32_t bit = 1u << (pin & 31u);
	if (on)
		REG32(GPIO_BASE + 0x00u + bank * 0x04u) = bit; /* GPSR */
	else
		REG32(GPIO_BASE + 0x08u + bank * 0x04u) = bit; /* GPCR */
}
