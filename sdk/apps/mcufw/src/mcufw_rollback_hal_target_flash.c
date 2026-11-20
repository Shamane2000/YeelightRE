/*
 * mcufw_rollback_hal_target_flash.c -- Real WMPT + sys_psm A/B rollback backend.
 *
 * Boot counter ("mcufw.boot_cnt") is stored safely inside the WMPT PSM
 * partition via sys_psm_init()/psm_get_variable()/psm_set_variable().
 * Passive slot activation uses real part_init()/rfget_get_passive_firmware()/
 * part_set_active_partition() + Cortex-M4 AIRCR reset.
 *
 * NOTE: psm-v2 uses os_rwlock_* FreeRTOS primitives, so callers MUST invoke
 * mcufw_rollback_early_boot_check() from runtime_task() after vTaskStartScheduler().
 */
#include "mcufw_rollback_hal.h"
#include "mcufw_regs.h"
#include "mcufw_wmsdk_api.h"

#define SCB_AIRCR_REG      0xE000ED0Cu
#define SCB_AIRCR_KEY      0x05FA0004u
#define MCUFW_PSM_BOOT_KEY "mcufw.boot_cnt"

static psm_hnd_t ensure_psm_handle(void)
{
	(void)wm_core_init();
	(void)sys_psm_init();
	return sys_psm_get_handle();
}

uint32_t mcufw_rollback_hal_read(void)
{
	psm_hnd_t h = ensure_psm_handle();
	uint32_t val = 0u;
	int rc;

	if (!h)
		return 0xFFFFFFFFu;
	rc = psm_get_variable(h, MCUFW_PSM_BOOT_KEY, &val, sizeof(val));
	if (rc < 0)
		return 0u; /* Variable absent on first boot */
	if (rc != (int)sizeof(val))
		return 0xFFFFFFFFu;
	return val;
}

int mcufw_rollback_hal_write(uint32_t count)
{
	psm_hnd_t h = ensure_psm_handle();

	if (!h)
		return -1;
	return psm_set_variable(h, MCUFW_PSM_BOOT_KEY, &count, sizeof(count)) == 0
		? 0 : -1;
}

int mcufw_rollback_hal_activate_passive_slot(void)
{
	struct partition_entry *pe;
	int16_t idx = 0;

	(void)wm_core_init();
	if (part_init() != 0)
		return -1;

	pe = rfget_get_passive_firmware();
	if (!pe) {
		struct partition_entry *p1 = part_get_layout_by_id(FC_COMP_FW, &idx);
		struct partition_entry *p2 = part_get_layout_by_id(FC_COMP_FW, &idx);
		if (p1 && p2)
			pe = part_get_passive_partition(p1, p2);
	}
	if (!pe || pe->type != (uint8_t)FC_COMP_FW || pe->size < 0x20000u)
		return -1;

	return part_set_active_partition(pe) == 0 ? 0 : -1;
}

void mcufw_rollback_hal_reset(void)
{
	REG32(SCB_AIRCR_REG) = SCB_AIRCR_KEY;
	for (;;)
		;
}
