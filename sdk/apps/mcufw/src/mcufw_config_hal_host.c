/*
 * mcufw_config_hal_host.c -- persistent-storage seam, HOST backend (phase 4).
 *
 * Models the config sector as a byte array so the host test can assert the
 * exact serialised block, corrupt single bytes and count real backend writes
 * on x86 with no device access.  Same link-time pattern as mcufw_pwm_hal_host.
 *
 * Invariants kept here:
 *   - only whole blocks are transferred (short read/write = error)
 *   - an erased window reads back as "unreadable" (g_valid == 0), which is what
 *     a virgin flash sector looks like to mcufw_config_load()
 */
#include "mcufw_config.h"
#include "mcufw_config_hal.h"

static uint8_t  g_store[MCUFW_CONFIG_SIZE];
static uint32_t g_writes;
static uint32_t g_reads;
static int      g_valid;
static uint8_t  g_devcfg_store[256];
static size_t   g_devcfg_len;
static int      g_devcfg_valid;

int mcufw_config_hal_read(uint8_t *buf, size_t len)
{
	size_t i;

	g_reads++;
	if (!buf || len != MCUFW_CONFIG_SIZE || !g_valid)
		return -1;
	for (i = 0; i < len; i++)
		buf[i] = g_store[i];
	return 0;
}

int mcufw_config_hal_write(const uint8_t *buf, size_t len)
{
	size_t i;

	g_writes++;
	if (!buf || len != MCUFW_CONFIG_SIZE)
		return -1;
	for (i = 0; i < len; i++)
		g_store[i] = buf[i];
	g_valid = 1;
	return 0;
}

const char *mcufw_config_hal_name(void)
{
	return "host-array";
}

void mcufw_config_hal_stub_reset(void)
{
	size_t i;

	for (i = 0; i < MCUFW_CONFIG_SIZE; i++)
		g_store[i] = 0xffu;   /* erased NOR flash reads 0xFF */
	g_writes = 0u;
	g_reads = 0u;
	g_valid = 0;
	g_devcfg_len = 0u;
	g_devcfg_valid = 0;
	for (i = 0; i < sizeof(g_devcfg_store); i++) g_devcfg_store[i] = 0xffu;
}

void mcufw_config_hal_stub_invalidate(void)
{
	g_valid = 0;
}

uint8_t *mcufw_config_hal_stub_bytes(void)
{
	return g_store;
}

uint32_t mcufw_config_hal_stub_writes(void)
{
	return g_writes;
}

uint32_t mcufw_config_hal_stub_reads(void)
{
	return g_reads;
}

int mcufw_config_hal_stub_valid(void)
{
	return g_valid;
}

int mcufw_devcfg_hal_read(uint8_t *buf, size_t len)
{
	if (!buf || len == 0u || !g_devcfg_valid || len != g_devcfg_len)
		return -1;
	for (size_t i = 0; i < len; i++)
		buf[i] = g_devcfg_store[i];
	return 0;
}

int mcufw_devcfg_hal_write(const uint8_t *buf, size_t len)
{
	if (!buf || len == 0u || len > sizeof(g_devcfg_store))
		return -1;
	for (size_t i = 0; i < len; i++)
		g_devcfg_store[i] = buf[i];
	g_devcfg_len = len;
	g_devcfg_valid = 1;
	return 0;
}
