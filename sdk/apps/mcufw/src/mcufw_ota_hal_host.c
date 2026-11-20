/*
 * mcufw_ota_hal_host.c -- Deterministic RAM mock for host OTA unit tests.
 */
#include "mcufw_ota_hal.h"

#define MOCK_SLOT_CAP (64u * 1024u)

static const uint8_t *g_http_data;
static size_t   g_http_len;
static size_t   g_http_pos;
static int      g_http_open_rc;
static int      g_http_status;
static uint8_t  g_http_chunked;
static uint32_t g_http_declared_len;
static int      g_has_passive = 1;
static uint32_t g_slot_max = 0x60000u;

static uint8_t  g_slot_buf[MOCK_SLOT_CAP];
static size_t   g_slot_written;
static int      g_active_slot;

static uint32_t g_cnt_begin;
static uint32_t g_cnt_abort;
static uint32_t g_cnt_complete;
static uint32_t g_cnt_reboot;

static int h_open(const char *url, int *status_code,
                  uint32_t *content_len, uint8_t *chunked)
{
	(void)url;
	if (g_http_open_rc != 0) return -1;
	g_http_pos = 0u;
	if (status_code) *status_code = g_http_status;
	if (content_len) *content_len = g_http_declared_len;
	if (chunked)     *chunked = g_http_chunked;
	return 0;
}

static int h_read(void *buf, size_t max_len)
{
	size_t avail = (g_http_pos < g_http_len) ? (g_http_len - g_http_pos) : 0u;
	size_t n = avail < max_len ? avail : max_len;
	uint8_t *d = (uint8_t *)buf;
	for (size_t i = 0; i < n; i++) d[i] = g_http_data[g_http_pos++];
	return (int)n;
}

static void h_close(void) {}

static int h_slot_begin(uint32_t *slot_max_size)
{
	g_cnt_begin++;
	if (!g_has_passive) return -1;
	g_slot_written = 0u;
	if (slot_max_size) *slot_max_size = g_slot_max;
	return 0;
}

static int h_slot_write(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (g_slot_written < MOCK_SLOT_CAP)
			g_slot_buf[g_slot_written] = data[i];
		g_slot_written++;
	}
	return 0;
}

static int h_slot_complete(void)
{
	g_cnt_complete++;
	g_active_slot ^= 1;
	return 0;
}

static void h_slot_abort(void) { g_cnt_abort++; }
static void h_reboot(void) { g_cnt_reboot++; }

static const mcufw_ota_hal_t g_host_ota_hal = {
	h_open, h_read, h_close,
	h_slot_begin, h_slot_write, h_slot_complete, h_slot_abort,
	h_reboot
};

const mcufw_ota_hal_t *mcufw_ota_hal_default(void) { return &g_host_ota_hal; }

void mcufw_ota_hal_stub_reset(const uint8_t *payload, size_t payload_len,
                              int http_status, uint32_t declared_len,
                              uint8_t chunked, uint32_t slot_max)
{
	g_http_data = payload;
	g_http_len = payload_len;
	g_http_pos = 0u;
	g_http_open_rc = 0;
	g_http_status = http_status;
	g_http_declared_len = declared_len;
	g_http_chunked = chunked;
	g_has_passive = 1;
	g_slot_max = slot_max;
	g_slot_written = 0u;
	g_active_slot = 0;
	g_cnt_begin = 0u;
	g_cnt_abort = 0u;
	g_cnt_complete = 0u;
	g_cnt_reboot = 0u;
}

void mcufw_ota_hal_stub_set_open_err(int err) { g_http_open_rc = err; }
uint32_t mcufw_ota_hal_stub_begin_calls(void) { return g_cnt_begin; }
uint32_t mcufw_ota_hal_stub_abort_calls(void) { return g_cnt_abort; }
uint32_t mcufw_ota_hal_stub_complete_calls(void) { return g_cnt_complete; }
uint32_t mcufw_ota_hal_stub_reboot_calls(void) { return g_cnt_reboot; }
int      mcufw_ota_hal_stub_active_slot(void) { return g_active_slot; }
size_t   mcufw_ota_hal_stub_written(void) { return g_slot_written; }
const uint8_t *mcufw_ota_hal_stub_bytes(void) { return g_slot_buf; }
