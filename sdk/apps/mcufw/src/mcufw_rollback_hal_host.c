/*
 * mcufw_rollback_hal_host.c -- RAM-Stubs für Host-Tests (Phase 4b.1).
 *
 * Simuliert persistenten Counter + Partition-API im RAM.
 * Für deterministische Tests ohne Flash-Zugriff.
 */
#include "mcufw_rollback_hal.h"
#include <stdlib.h>

static uint32_t g_counter = 0u;
static int g_passive_activated = 0;
static int g_reset_called = 0;
static int g_force_read_error = 0;
static int g_force_write_error = 0;
static int g_force_activate_error = 0;

uint32_t mcufw_rollback_hal_read(void)
{
	if (g_force_read_error)
		return 0xFFFFFFFFu;
	return g_counter;
}

int mcufw_rollback_hal_write(uint32_t count)
{
	if (g_force_write_error)
		return -1;
	g_counter = count;
	return 0;
}

int mcufw_rollback_hal_activate_passive_slot(void)
{
	if (g_force_activate_error)
		return -1;
	g_passive_activated = 1;
	return 0;
}

void mcufw_rollback_hal_reset(void)
{
	g_reset_called = 1;
	/* Host-Test stub: returns immediately. Real target uses NVIC_SystemReset(). */
}

/* --- Test-Hilfsfunktionen (nur Host) ----------------------------------- */
void mcufw_rollback_test_reset_state(void)
{
	g_counter = 0u;
	g_passive_activated = 0;
	g_reset_called = 0;
	g_force_read_error = 0;
	g_force_write_error = 0;
	g_force_activate_error = 0;
}

int mcufw_rollback_test_was_passive_activated(void)
{
	return g_passive_activated;
}

int mcufw_rollback_test_was_reset_called(void)
{
	return g_reset_called;
}

void mcufw_rollback_test_force_read_error(int enable)
{
	g_force_read_error = enable;
}

void mcufw_rollback_test_force_write_error(int enable)
{
	g_force_write_error = enable;
}

void mcufw_rollback_test_force_activate_error(int enable)
{
	g_force_activate_error = enable;
}
