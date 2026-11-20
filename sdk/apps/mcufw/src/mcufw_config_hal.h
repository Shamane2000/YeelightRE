/*
 * mcufw_config_hal.h -- persistent-storage seam (phase 4).
 *
 * Same pattern as mcufw_pwm_hal.h: one named layer between "what must be
 * remembered" (mcufw_config.c) and "where the bytes live".  The backend is
 * chosen at LINK time by the Makefile, never by an #ifdef in a caller.
 *
 *   host            src/mcufw_config_hal_host.c         byte array + counters
 *   target (stub)   src/mcufw_config_hal_target_stub.c  no persistence, loud
 *   target (flash)  src/mcufw_config_hal_target_flash.c flash_drv_* (opt-in)
 *
 * The host-only inspection symbols are defined by the host backend alone, so
 * nothing that ends up in the firmware image can depend on the stub window
 * (nm guard `check-owner-elf`).
 */
#ifndef MCUFW_CONFIG_HAL_H
#define MCUFW_CONFIG_HAL_H

#include <stddef.h>
#include <stdint.h>

/* Read/write the config block.  Both return 0 on success, negative on error;
 * a short transfer is an error (no partial blocks are ever accepted). */
int mcufw_config_hal_read(uint8_t *buf, size_t len);
int mcufw_config_hal_write(const uint8_t *buf, size_t len);

/* Backend identity for the UART banner and the host-test sanity check. */
const char *mcufw_config_hal_name(void);

int mcufw_devcfg_hal_read(uint8_t *buf, size_t len);
int mcufw_devcfg_hal_write(const uint8_t *buf, size_t len);

/* ---- host backend only -------------------------------------------------- */
void     mcufw_config_hal_stub_reset(void);      /* erase window + counters  */
void     mcufw_config_hal_stub_invalidate(void); /* mark window unreadable   */
uint8_t *mcufw_config_hal_stub_bytes(void);      /* raw window, for corruption */
uint32_t mcufw_config_hal_stub_writes(void);     /* backend write count      */
uint32_t mcufw_config_hal_stub_reads(void);      /* backend read count       */
int      mcufw_config_hal_stub_valid(void);      /* 1 = window holds bytes   */

#endif /* MCUFW_CONFIG_HAL_H */
