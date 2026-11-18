/*
 * mcufw_config_hal_target_stub.c -- DECLARED STUB, target default (phase 4).
 *
 * THIS BACKEND DOES NOT PERSIST ANYTHING.  That is deliberate and loud:
 *
 *   name()  -> "stub-no-persist"   (printed in the boot banner)
 *   read()  -> -1 always           (every boot falls back to the defaults)
 *   write() -> counts, discards    (no flash touched, no wear, no lie)
 *
 * Why not flash already?  flash_drv_open/read/write/erase really exist in
 * libwmsdk.a (see sdk/vendor/.../sdk/flash.h) and spi_flash_* in
 * drivers/mw300/mw300_spi_flash.h, but there is NO verified free sector for a
 * config block: layout.txt marks everything from 0x070000 as "ungenutzt /
 * reserviert" without a partition table entry, and writing into an unproven
 * offset could brick the lamp.  Proving a safe sector + erase/write cycle on
 * the bench carrier is phase 5 (OTA + rollback protection), not phase 4.
 *
 * Selecting the real backend is an explicit build decision:
 *   make MCUFW_CONFIG_BACKEND=flash
 * which links mcufw_config_hal_target_flash.c instead of this file.
 */
#include "mcufw_config.h"
#include "mcufw_config_hal.h"

static uint32_t g_stub_writes;
static uint32_t g_stub_reads;

int mcufw_config_hal_read(uint8_t *buf, size_t len)
{
	(void)buf;
	(void)len;
	g_stub_reads++;
	return -1;   /* nothing stored: caller must use its defaults */
}

int mcufw_config_hal_write(const uint8_t *buf, size_t len)
{
	(void)buf;
	(void)len;
	g_stub_writes++;
	return 0;    /* accepted and dropped -- see header comment */
}

const char *mcufw_config_hal_name(void)
{
	return "stub-no-persist";
}
