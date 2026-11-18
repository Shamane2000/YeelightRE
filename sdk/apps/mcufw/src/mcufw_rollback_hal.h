/*
 * mcufw_rollback_hal.h -- HAL-Seam für Rollback-Counter-Backend.
 *
 * Link-Time-Selektion:
 *   MCUFW_ROLLBACK_BACKEND=stub  -> mcufw_rollback_hal_host.c (RAM-Array)
 *   MCUFW_ROLLBACK_BACKEND=flash -> mcufw_rollback_hal_target_flash.c (WMPT-Sektor)
 *
 * Das Target-Flash-Backend bleibt STUB, bis Sektor-Mapping in tools/ verifiziert.
 */
#ifndef MCUFW_ROLLBACK_HAL_H
#define MCUFW_ROLLBACK_HAL_H

#include <stdint.h>

/* Liest den persistenten Boot-Fail-Counter.
 * Rückgabe: 0..UINT32_MAX, oder 0xFFFFFFFF bei Lesefehler. */
uint32_t mcufw_rollback_hal_read(void);

/* Schreibt den Boot-Fail-Counter persistent.
 * Rückgabe: 0 = OK, !=0 = Fehler. */
int mcufw_rollback_hal_write(uint32_t count);

/* Aktiviert den passiven App-Slot via WMSDK Partition-API.
 * Rückgabe: 0 = OK, !=0 = Fehler. */
int mcufw_rollback_hal_activate_passive_slot(void);

/* Führt einen MCU-Reset aus (NVIC_SystemReset oder äquivalent).
 * Kehrt NICHT zurück. */
void mcufw_rollback_hal_reset(void);

#endif /* MCUFW_ROLLBACK_HAL_H */
