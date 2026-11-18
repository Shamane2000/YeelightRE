/*
 * mcufw_rollback_hal_target_flash.c -- WMPT-Sektor-Backend (Phase 4b.1).
 *
 * WARNUNG: Dieses Backend ist PLACEHOLDER, bis das exakte WMPT-Sektor-Mapping
 * in tools/ verifiziert ist. Derzeit aktiviert es den passiven Slot NICHT echt
 * und resettet nicht – stattdessen kehrt es mit Fehler zurück, damit main()
 * den Boot fortsetzt (Fail-Open statt Brick).
 *
 * Phase-5-Aktivierung:
 *   1. Sektor-Adresse + Größe in tools/wmpt_layout.py bestimmen.
 *   2. flash_drv_read/write/erase gegen echte Adresse testen.
 *   3. part_init / part_get_passive_partition / part_set_active_partition
 *      gegen libwmsdk.a validieren.
 *   4. Dieses File auf echte Calls umschalten.
 */
#include "mcufw_rollback_hal.h"

/* Placeholder: keine echten Flash-Calls bis Sektor verifiziert. */

uint32_t mcufw_rollback_hal_read(void)
{
	/* TODO: flash_drv_read(WMPT_COUNTER_SECTOR, ...) */
	return 0u;
}

int mcufw_rollback_hal_write(uint32_t count)
{
	(void)count;
	/* TODO: flash_drv_erase + flash_drv_write */
	return 0;
}

int mcufw_rollback_hal_activate_passive_slot(void)
{
	/* TODO: part_init(); part_get_passive_partition(); part_set_active_partition() */
	return -1; /* Fail-open: kein echter Swap bis verifiziert. */
}

void mcufw_rollback_hal_reset(void)
{
	/* TODO: NVIC_SystemReset() oder SCB->AIRCR */
	for (;;)
		;
}
