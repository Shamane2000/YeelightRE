/* mcufw_pwm_hal_host.c -- GPT0 + GPIO seam, host backend (phase 4b.1). */
#include "mcufw_pwm_hal.h"
#include <string.h>

#define STUB_USER_REQ    0x0f0u
#define STUB_CH_BASE(ch) (0x200u + 0x30u * (ch))
#define STUB_CH_CMR0     0x10u
#define STUB_CH_COUNT    6u
#define STUB_CMR_UPDT(ch) (1u << (16u + (ch)))
#define STUB_GPIO_COUNT  64u

static uint32_t g_regs[MCUFW_PWM_HAL_WORDS];
static uint32_t g_writes;
static uint32_t g_bad_offset;
static uint32_t g_latched;
static uint32_t g_latch_errors;
static unsigned g_pending_ch;
static int      g_pending_armed;
static uint8_t  g_gpio[STUB_GPIO_COUNT];

static int word_index(uint32_t offset, uint32_t *idx)
{
	if ((offset & 3u) != 0u || offset >= MCUFW_PWM_HAL_SPAN)
		return -1;
	*idx = offset >> 2;
	return 0;
}

static int cmr0_channel(uint32_t offset)
{
	unsigned ch;
	for (ch = 0; ch < STUB_CH_COUNT; ch++) {
		if (offset == STUB_CH_BASE(ch) + STUB_CH_CMR0)
			return (int)ch;
	}
	return -1;
}

void mcufw_pwm_hal_write(uint32_t offset, uint32_t value)
{
	uint32_t idx;
	int ch;

	g_writes++;
	if (word_index(offset, &idx) != 0) {
		g_bad_offset++;
		return;
	}
	g_regs[idx] = value;

	ch = cmr0_channel(offset);
	if (ch >= 0) {
		g_pending_ch = (unsigned)ch;
		g_pending_armed = 1;
	} else if (offset == STUB_USER_REQ) {
		if (g_pending_armed && value == STUB_CMR_UPDT(g_pending_ch))
			g_latched |= 1u << g_pending_ch;
		else
			g_latch_errors++;
		g_pending_armed = 0;
	}
}

uint32_t mcufw_pwm_hal_read(uint32_t offset)
{
	uint32_t idx;
	if (word_index(offset, &idx) != 0) {
		g_bad_offset++;
		return 0u;
	}
	return g_regs[idx];
}

const char *mcufw_pwm_hal_name(void) { return "stub-array"; }

void mcufw_pwm_hal_gpio_set(uint8_t pin, uint8_t on)
{
	if (pin < STUB_GPIO_COUNT)
		g_gpio[pin] = on ? 1u : 0u;
}

uint8_t mcufw_pwm_hal_stub_gpio_get(uint8_t pin)
{
	if (pin < STUB_GPIO_COUNT)
		return g_gpio[pin];
	return 0u;
}

void mcufw_pwm_hal_stub_reset(void)
{
	memset(g_regs, 0, sizeof(g_regs));
	memset(g_gpio, 0, sizeof(g_gpio));
	g_writes = 0u;
	g_bad_offset = 0u;
	g_latched = 0u;
	g_latch_errors = 0u;
	g_pending_ch = 0u;
	g_pending_armed = 0;
}

uint32_t mcufw_pwm_hal_stub_read(uint32_t offset) { return mcufw_pwm_hal_read(offset); }
uint32_t mcufw_pwm_hal_stub_writes(void) { return g_writes; }
uint32_t mcufw_pwm_hal_stub_bad_offset(void) { return g_bad_offset; }
uint32_t mcufw_pwm_hal_stub_latched_mask(void) { return g_latched; }
uint32_t mcufw_pwm_hal_stub_latch_errors(void) { return g_latch_errors; }
