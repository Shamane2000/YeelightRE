/*
 * mcufw_config_hal_target_flash.c -- OPT-IN real flash backend (phase 4 code,
 * phase 5 activation).  Linked only with MCUFW_CONFIG_BACKEND=flash.
 *
 * Uses the vendor mdev flash API that is genuinely present in libwmsdk.a:
 *   flash_drv_open(FL_SPI_EXT) / _read / _erase / _write / _close
 * (sdk/vendor/ez-connect-lite/sdk/src/incl/sdk/flash.h:118..175,
 *  low level: .../drivers/mw300/mw300_spi_flash.h:88..96)
 *
 * NOT ENABLED BY DEFAULT AND NOT YET PROVEN ON SILICON.  The sector address
 * below is a CANDIDATE inside the region layout.txt calls unused/reserved
 * (>= 0x070000); it must be confirmed against the real partition table and an
 * erase/write/read cycle on the bench lamp before this backend may ship.
 * Until then the default build links the declared stub, so nothing silently
 * pretends the lamp remembers its state.
 *
 * Write discipline: one 4 KiB sector erase per changed block, never per tick
 * (mcufw_config_save() already skips identical blocks).
 */
#include "mcufw_config.h"
#include "mcufw_config_hal.h"

#include "flash.h"

/* Candidate sector: first 4 KiB block above the 384 KiB app slot. */
#ifndef MCUFW_CONFIG_FLASH_DEV
#define MCUFW_CONFIG_FLASH_DEV FL_SPI_EXT
#endif
#ifndef MCUFW_CONFIG_FLASH_ADDR
#define MCUFW_CONFIG_FLASH_ADDR 0x070000ul
#endif
#ifndef MCUFW_CONFIG_FLASH_SECTOR
#define MCUFW_CONFIG_FLASH_SECTOR 0x1000ul
#endif

int mcufw_config_hal_read(uint8_t *buf, size_t len)
{
	mdev_t *dev;
	int rc;

	if (!buf || len != MCUFW_CONFIG_SIZE)
		return -1;

	dev = flash_drv_open(MCUFW_CONFIG_FLASH_DEV);
	if (!dev)
		return -1;
	rc = flash_drv_read(dev, buf, (uint32_t)len,
			    (uint32_t)MCUFW_CONFIG_FLASH_ADDR);
	flash_drv_close(dev);
	return rc == 0 ? 0 : -1;
}

int mcufw_config_hal_write(const uint8_t *buf, size_t len)
{
	mdev_t *dev;
	int rc;

	if (!buf || len != MCUFW_CONFIG_SIZE)
		return -1;

	dev = flash_drv_open(MCUFW_CONFIG_FLASH_DEV);
	if (!dev)
		return -1;
	rc = flash_drv_erase(dev, MCUFW_CONFIG_FLASH_ADDR,
			     MCUFW_CONFIG_FLASH_SECTOR);
	if (rc == 0)
		rc = flash_drv_write(dev, buf, (uint32_t)len,
				     (uint32_t)MCUFW_CONFIG_FLASH_ADDR);
	flash_drv_close(dev);
	return rc == 0 ? 0 : -1;
}

const char *mcufw_config_hal_name(void)
{
	return "flash-spi-ext";
}
