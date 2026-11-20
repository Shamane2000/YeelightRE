/*
 * mcufw_rollback.h -- Early-Boot A/B-Slot-Rollback (Phase 4b.1).
 *
 * Schutz vor Boot-Loops nach fehlerhaftem OTA-Flash:
 *   - boot_fail_count wird bei JEDEM Kaltstart VOR WLAN/RTOS-Init inkrementiert.
 *   - Bei >= MCUFW_ROLLBACK_THRESHOLD (5) schnellen Power-Cycles:
 *       Counter reset + passiven App-Slot aktivieren + MCU-Reset.
 *   - Nach stabilem Lauf (WLAN-Join + Recovery-Timer) wird
 *     mcufw_rollback_confirm_good() aufgerufen -> Counter = 0.
 *
 * Das Flash-/Counter-Backend ist link-time gekapselt (stub|flash),
 * damit Host-Tests deterministisch laufen und echter Flash-Zugriff
 * erst nach Sektor-Verifikation in Phase 5 opt-in aktiviert wird.
 */
#ifndef MCUFW_ROLLBACK_H
#define MCUFW_ROLLBACK_H

#include <stdint.h>

/* Anzahl fehlgeschlagener Boots, bevor Rollback ausgelöst wird. */
#define MCUFW_ROLLBACK_THRESHOLD 5u

/* Ergebnis der Early-Boot-Prüfung. */
typedef enum {
    MCUFW_RB_OK          = 0, /* Counter < Threshold, normaler Boot */
    MCUFW_RB_ROLLED_BACK = 1, /* Threshold erreicht, Slot getauscht + Reset */
    MCUFW_RB_ERROR       = 2  /* Backend-Fehler (korrupt/IO), sicherheitshalber Rollback */
} mcufw_rollback_result_t;

/* MUSS als allererstes in main() vor jeglicher RTOS/WLAN-Init aufgerufen werden.
 * Inkrementiert boot_fail_count, prüft gegen Threshold.
 * Kehrt nur zurück, wenn Boot OK ist; bei Rollback wird intern reset ausgeführt
 * (kehrt also nie zurück). Bei Backend-Fehler: versucht trotzdem Rollback. */
mcufw_rollback_result_t mcufw_rollback_early_boot_check(void);

/* Setzt boot_fail_count auf 0. Erst NACH stabilem Lauf aufrufen
 * (WLAN assoziiert + DHCP + Recovery-Endpunkt >= 15s stabil). */
void mcufw_rollback_confirm_good(void);

/* Nur für Host-Tests: liest aktuellen Counter-Stand ohne Seiteneffekte. */
uint32_t mcufw_rollback_read_counter(void);

/* Nur für Host-Tests: setzt Counter explizit (Power-Loss-Simulation). */
void mcufw_rollback_set_counter(uint32_t count);

#define MCUFW_ROLLBACK_CONFIRM_TICKS 1500u /* ~15s bei 10ms/Tick */

/* Verzoegerte Boot-Bestaetigung im Runtime-Loop (ruft confirm_good() einmalig
 * auf, sobald wlan_ready && *ticks >= threshold_ticks). */
int mcufw_rollback_deferred_tick(uint32_t *ticks, int *confirmed,
                                 int wlan_ready, uint32_t threshold_ticks);

#endif /* MCUFW_ROLLBACK_H */
