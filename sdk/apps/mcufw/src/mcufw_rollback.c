/*
 * mcufw_rollback.c -- Early-Boot A/B-Slot-Rollback (Phase 4b.1).
 *
 * Siehe mcufw_rollback.h für Design & Vertrag.
 * Dieses Modul DARF keine RTOS-/WLAN-/lwIP-Symbole referenzieren,
 * da es vor jeglicher System-Init läuft.
 */
#include "mcufw_rollback.h"
#include "mcufw_rollback_hal.h"

mcufw_rollback_result_t mcufw_rollback_early_boot_check(void)
{
	uint32_t count = mcufw_rollback_hal_read();

	/* Lesefehler oder korrupter Sektor: sicherheitshalber Rollback versuchen. */
	if (count == 0xFFFFFFFFu) {
		(void)mcufw_rollback_hal_write(0u);
		if (mcufw_rollback_hal_activate_passive_slot() == 0) {
			mcufw_rollback_hal_reset();
		}
		return MCUFW_RB_ERROR;
	}

	count++;
	if (mcufw_rollback_hal_write(count) != 0) {
		/* Schreibfehler: trotzdem Rollback versuchen, falls Threshold erreicht. */
		if (count >= MCUFW_ROLLBACK_THRESHOLD) {
			(void)mcufw_rollback_hal_write(0u);
			if (mcufw_rollback_hal_activate_passive_slot() == 0) {
				mcufw_rollback_hal_reset();
			}
			return MCUFW_RB_ERROR;
		}
		return MCUFW_RB_ERROR;
	}

	if (count >= MCUFW_ROLLBACK_THRESHOLD) {
		/* Threshold erreicht: Counter resetten, passiven Slot aktivieren, Reset. */
		(void)mcufw_rollback_hal_write(0u);
		if (mcufw_rollback_hal_activate_passive_slot() == 0) {
			mcufw_rollback_hal_reset();
			/* noreturn – kommt nie hier an. */
		}
		/* Aktivierung fehlgeschlagen: Fehler melden, aber Boot fortsetzen
		 * (besser als Brick; User kann erneut power-cyclen). */
		return MCUFW_RB_ERROR;
	}

	return MCUFW_RB_OK;
}

void mcufw_rollback_confirm_good(void)
{
	(void)mcufw_rollback_hal_write(0u);
}

uint32_t mcufw_rollback_read_counter(void)
{
	return mcufw_rollback_hal_read();
}

void mcufw_rollback_set_counter(uint32_t count)
{
	(void)mcufw_rollback_hal_write(count);
}
