/* mcufw_pwm_hal_host.c -- GPT0 register seam, host backend (phase 3.7).
 *
 * Models the GPT0 register window as a word array so the host test can assert
 * the exact offsets and values the driver produced, on x86, with no device
 * access.  Offsets are the same ones the target backend feeds into
 * GPT0_BASE + off.
 *
 * Beyond the plain array the stub keeps two invariants the driver must honour:
 *   - every access stays inside the modelled window (bad_offset)
 *   - every CMR0 write is followed by the matching CHn_CMR_UPDT request in
 *     USER_REQ, i.e. the new match value really latches on the next period
 *     instead of tearing mid-period (latched_mask / latch_errors)
 */
#include "mcufw_pwm_hal.h"

/* GPT0 layout knowledge, identical to mcufw_regs.h.  Kept local so the stub
 * stays a self-contained model of the peripheral. */
#define STUB_USER_REQ    0x0f0u
#define STUB_CH_BASE(ch) (0x200u + 0x30u * (ch))
#define STUB_CH_CMR0     0x10u
#define STUB_CH_COUNT    6u
#define STUB_CMR_UPDT(ch) (1u << (16u + (ch)))

static uint32_t g_regs[MCUFW_PWM_HAL_WORDS];
static uint32_t g_writes;
static uint32_t g_bad_offset;
static uint32_t g_latched;
static uint32_t g_latch_errors;
static unsigned g_pending_ch;
static int      g_pending_armed;

/* Map a byte offset to a word index; reject unaligned/out-of-window access so
 * a driver bug shows up as a counter instead of silent memory corruption. */
static int word_index(uint32_t offset, uint32_t *idx)
{
	if ((offset & 3u) != 0u || offset >= MCUFW_PWM_HAL_SPAN)
		return -1;
	*idx = offset >> 2;
	return 0;
}

/* Channel index if the offset is that channel's CMR0, else -1. */
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

const char *mcufw_pwm_hal_name(void)
{
	return "stub-array";
}

void mcufw_pwm_hal_stub_reset(void)
{
	uint32_t i;

	for (i = 0; i < MCUFW_PWM_HAL_WORDS; i++)
		g_regs[i] = 0u;
	g_writes = 0u;
	g_bad_offset = 0u;
	g_latched = 0u;
	g_latch_errors = 0u;
	g_pending_ch = 0u;
	g_pending_armed = 0;
}

uint32_t mcufw_pwm_hal_stub_read(uint32_t offset)
{
	return mcufw_pwm_hal_read(offset);
}

uint32_t mcufw_pwm_hal_stub_writes(void)
{
	return g_writes;
}

uint32_t mcufw_pwm_hal_stub_bad_offset(void)
{
	return g_bad_offset;
}

uint32_t mcufw_pwm_hal_stub_latched_mask(void)
{
	return g_latched;
}

uint32_t mcufw_pwm_hal_stub_latch_errors(void)
{
	return g_latch_errors;
}
