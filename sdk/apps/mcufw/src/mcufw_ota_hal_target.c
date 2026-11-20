/*
 * mcufw_ota_hal_target.c -- Real WMSDK httpc + rfget passive-slot OTA HAL.
 */
#include "mcufw_ota_hal.h"
#include "mcufw_rollback_hal.h"
#include "mcufw_wmsdk_api.h"

static http_session_t g_sess;
static int g_sess_open;
static update_desc_t g_ud;
static int g_ud_open;

static int t_open(const char *url, int *status_code,
                  uint32_t *content_len, uint8_t *chunked)
{
	mcufw_http_resp_t *resp = NULL;
	int rc;

	if (g_sess_open)
		http_close_session(&g_sess);
	g_sess = 0;
	g_sess_open = 0;

	rc = httpc_get(url, &g_sess, &resp, NULL);
	if (rc != 0 || !resp)
		return -1;
	g_sess_open = 1;
	if (status_code) *status_code = resp->status_code;
	if (content_len) *content_len = resp->content_length;
	if (chunked)     *chunked = resp->chunked;
	return 0;
}

static int t_read(void *buf, size_t max_len)
{
	if (!g_sess_open) return -1;
	return http_read_content(g_sess, buf, (uint32_t)max_len);
}

static void t_close(void)
{
	if (g_sess_open) {
		http_close_session(&g_sess);
		g_sess_open = 0;
	}
}

static int t_slot_begin(uint32_t *slot_max_size)
{
	struct partition_entry *pe;

	(void)wm_core_init();
	(void)part_init();
	(void)rfget_init();
	pe = rfget_get_passive_firmware();
	if (!pe || pe->type != (uint8_t)FC_COMP_FW)
		return -1;
	if (rfget_update_begin(&g_ud, pe) != 0)
		return -1;
	g_ud_open = 1;
	if (slot_max_size) *slot_max_size = pe->size;
	return 0;
}

static int t_slot_write(const uint8_t *data, size_t len)
{
	if (!g_ud_open) return -1;
	return rfget_update_data(&g_ud, (const char *)data, len) == 0 ? 0 : -1;
}

static int t_slot_complete(void)
{
	int rc;
	if (!g_ud_open) return -1;
	g_ud_open = 0;
	rc = rfget_update_complete(&g_ud);
	return rc == 0 ? 0 : -1;
}

static void t_slot_abort(void)
{
	if (g_ud_open) {
		g_ud_open = 0;
		(void)rfget_update_abort(&g_ud);
	}
}

static void t_reboot(void)
{
	mcufw_rollback_hal_reset();
}

static const mcufw_ota_hal_t g_target_ota_hal = {
	t_open, t_read, t_close,
	t_slot_begin, t_slot_write, t_slot_complete, t_slot_abort,
	t_reboot
};

const mcufw_ota_hal_t *mcufw_ota_hal_default(void)
{
	return &g_target_ota_hal;
}
