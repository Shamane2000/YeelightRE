/*
 * mcufw_rollback_hal_target_stub.c -- Deklarierter No-Flash-Stub für das
 * ARM-Target bei MCUFW_ROLLBACK_BACKEND=stub (Phase 4b.2 Default bis WMPT/PSM
 * Sektor-Verifikation in Phase 4b.3). Kein <stdio.h>, kein Host-Mock-Code.
 */
#include "mcufw_rollback_hal.h"
#include "mcufw_regs.h"

#define SCB_AIRCR_REG   0xE000ED0Cu
#define SCB_AIRCR_KEY   0x05FA0004u

uint32_t mcufw_rollback_hal_read(void)
{
	return 0u;
}

int mcufw_rollback_hal_write(uint32_t count)
{
	(void)count;
	return 0;
}

int mcufw_rollback_hal_activate_passive_slot(void)
{
	return -1;
}

void mcufw_rollback_hal_reset(void)
{
	REG32(SCB_AIRCR_REG) = SCB_AIRCR_KEY;
	for (;;)
		;
}
