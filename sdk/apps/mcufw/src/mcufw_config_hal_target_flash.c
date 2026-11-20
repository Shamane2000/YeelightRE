/*
 * mcufw_config_hal_target_flash.c -- Real sys_psm persistent state backend.
 *
 * Uses Marvell's wear-leveled PSM partition discovered dynamically via WMPT
 * (FC_COMP_PSM -> sys_psm_init / psm_get_variable / psm_set_variable) under
 * key "mcufw.cfg". Zero hardcoded raw Flash offsets.
 */
#include "mcufw_config.h"
#include "mcufw_config_hal.h"
#include "mcufw_wmsdk_api.h"

#define MCUFW_PSM_CFG_KEY "mcufw.cfg"

static psm_hnd_t ensure_psm_handle(void)
{
	(void)wm_core_init();
	(void)sys_psm_init();
	return sys_psm_get_handle();
}

int mcufw_config_hal_read(uint8_t *buf, size_t len)
{
	psm_hnd_t h;
	int rc;

	if (!buf || len != MCUFW_CONFIG_SIZE)
		return -1;
	h = ensure_psm_handle();
	if (!h)
		return -1;
	rc = psm_get_variable(h, MCUFW_PSM_CFG_KEY, buf, (unsigned long)len);
	return (rc == (int)len) ? 0 : -1;
}

int mcufw_config_hal_write(const uint8_t *buf, size_t len)
{
	psm_hnd_t h;

	if (!buf || len != MCUFW_CONFIG_SIZE)
		return -1;
	h = ensure_psm_handle();
	if (!h)
		return -1;
	return (psm_set_variable(h, MCUFW_PSM_CFG_KEY, buf, (unsigned long)len) == 0)
		? 0 : -1;
}

const char *mcufw_config_hal_name(void)
{
	return "psm-wmpt";
}

#define MCUFW_PSM_DEVCFG_KEY "mcufw.devcfg"

int mcufw_devcfg_hal_read(uint8_t *buf, size_t len)
{
	psm_hnd_t h;
	int rc;

	if (!buf || len == 0u)
		return -1;
	h = ensure_psm_handle();
	if (!h)
		return -1;
	rc = psm_get_variable(h, MCUFW_PSM_DEVCFG_KEY, buf, (unsigned long)len);
	return (rc == (int)len) ? 0 : -1;
}

int mcufw_devcfg_hal_write(const uint8_t *buf, size_t len)
{
	psm_hnd_t h;

	if (!buf || len == 0u)
		return -1;
	h = ensure_psm_handle();
	if (!h)
		return -1;
	return (psm_set_variable(h, MCUFW_PSM_DEVCFG_KEY, buf, (unsigned long)len) == 0)
		? 0 : -1;
}
