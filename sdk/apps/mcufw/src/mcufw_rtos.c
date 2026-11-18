/*
 * mcufw_rtos.c -- FreeRTOS bring-up for mcufw phase 3.5.
 *
 * Replaces the inert shims from phase 3.4 with real scheduler start and
 * task creation. mcufw_os_ready() now returns 1 once the kernel is running,
 * which unblocks mcufw_wlan_bringup() stage 3 (mlan_init_fw).
 *
 * The heap uses FreeRTOS heap_4 (linked via Makefile); the bump allocator
 * shim is gone. vPortFree is now real.
 */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

static volatile int os_ready_flag;

int mcufw_os_ready(void)
{
	return os_ready_flag;
}

/* Called from main() after all pre-scheduler init is done. Does not return. */
void mcufw_rtos_start(void)
{
	os_ready_flag = 1;
	vTaskStartScheduler();
	/* Should never reach here. */
	for (;;)
		;
}
