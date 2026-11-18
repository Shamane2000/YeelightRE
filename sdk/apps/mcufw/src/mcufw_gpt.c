/* mcufw_gpt.c — GPT0 PWM lamp driver (register level).
 *
 * Reference path in color1_0061.elf:
 *   - lamp init table @0x1f00f1c4: four entries {pin, channel, gpt, fsel,
 *     period}; pins 0/1/4/5, channels 0/1/4/5, gpt 0, fsel 1, period 0xC350.
 *   - the app then calls the vendor PWM dispatch FUN_1f0110a0 (mode 1/2/3,
 *     vtable slot +0x44) from the 200 ms led timer; brightness is handed to
 *     the driver as 0xC350 - level, i.e. the LED string is low-active
 *     (matched/dark while the channel output is high).
 *
 * What is PROVEN and what is ASSUMED (see docs/09_button_gpt0_startup.md):
 *   proven   : GPT0 base 0x46070000, channel block 0x200 + ch*0x30, CMR0 at
 *              +0x10, CH_IO PWM edge = 6, UPPVAL = 0xC350, inverted duty
 *   assumed  : GPT0 clock source/divider/prescaler (CLK_CNTL bits) -- the app
 *              leaves this to the SDK's GPT_Init(), whose config struct is not
 *              present in .data, so mcufw programs divider/prescaler = 0 and
 *              documents the choice instead of inventing a value.
 */
#include "mcufw_regs.h"
#include "mcufw_gpt.h"
#include "mcufw_pwm_hal.h"

#define MCUFW_GPT0_CLK_DIV   0u   /* CLK_DIV [11:8]:  CLK / 2^div           */
#define MCUFW_GPT0_CLK_PRE   0u   /* CLK_PRE [23:16]: CLK / (pre + 1)       */

static const uint8_t lamp_ch[MCUFW_LAMP_CHANNELS] = { 0, 1, 4, 5 };

static uint16_t lamp_level;

/* Phase 3.7: every GPT0 access goes through the register seam, so the host
 * test can watch this driver's init/latch sequence in the stub array and the
 * firmware gets the same code with a plain MMIO pointer behind it. */
static void gpt0_write(uint32_t offset, uint32_t value)
{
	mcufw_pwm_hal_write(offset, value);
}

static uint32_t gpt0_read(uint32_t offset)
{
	return mcufw_pwm_hal_read(offset);
}

void mcufw_gpt0_pwm_init(void)
{
	unsigned i;

	/* Enable the GPT0 clock gate (see CSU_GATE_GPT0 note in mcufw_regs.h). */
	CSU_CLK_GATE &= ~CSU_GATE_GPT0;

	/* Counter clock: source 0, no divider, no prescaler -- see file header. */
	gpt0_write(GPT_CLK_CNTL, (0u << GPT_CLK_SRC) |
				 (MCUFW_GPT0_CLK_DIV << GPT_CLK_DIV) |
				 (MCUFW_GPT0_CLK_PRE << GPT_CLK_PRE));

	/* Period and counter update mode (normal auto-update, like the SDK). */
	gpt0_write(GPT_CNT_CNTL, (0u << GPT_CNT_UPDT_MOD));
	gpt0_write(GPT_UPPVAL, MCUFW_LAMP_PERIOD);

	/* Reset the counter and wait (bounded) for CNT_RST_DONE. */
	gpt0_write(GPT_CNT_EN, GPT_CNT_RESET);
	for (i = 0; i < 1000u; i++) {
		if (gpt0_read(GPT_CNT_EN) & GPT_CNT_RST_DONE)
			break;
	}

	/* One channel per lamp string: edge-aligned PWM, inverted polarity,
	 * duty 0 (CMR0 == period). */
	for (i = 0; i < MCUFW_LAMP_CHANNELS; i++) {
		uint32_t base = GPT_CHAN(lamp_ch[i]);

		gpt0_write(base + GPT_CH_CMR0, MCUFW_LAMP_PERIOD);
		gpt0_write(base + GPT_CH_CMR1, 0u);
		gpt0_write(base + GPT_CH_CNTL,
			   GPT_CH_IO_PWM_EDGE | GPT_CH_POL);
	}

	lamp_level = 0;
	gpt0_write(GPT_CNT_EN, GPT_CNT_START);
}

void mcufw_lamp_set(uint16_t level)
{
	uint32_t cmr;
	unsigned i;

	if (level > MCUFW_LAMP_PERIOD)
		level = MCUFW_LAMP_PERIOD;
	lamp_level = level;

	/* Low-active string: full brightness == CMR0 0, dark == CMR0 period. */
	cmr = (uint32_t)MCUFW_LAMP_PERIOD - (uint32_t)level;

	for (i = 0; i < MCUFW_LAMP_CHANNELS; i++) {
		gpt0_write(GPT_CHAN(lamp_ch[i]) + GPT_CH_CMR0, cmr);
		/* Latch the new match value on the next counter period. */
		gpt0_write(GPT_USER_REQ, GPT_CMR_UPDT(lamp_ch[i]));
	}
}

uint16_t mcufw_lamp_get(void)
{
	return lamp_level;
}

uint32_t mcufw_lamp_cmr0(unsigned index)
{
	if (index >= MCUFW_LAMP_CHANNELS)
		return 0u;

	return gpt0_read(GPT_CHAN(lamp_ch[index]) + GPT_CH_CMR0);
}

uint32_t mcufw_lamp_counter(void)
{
	return gpt0_read(GPT_CNT_VAL);
}
