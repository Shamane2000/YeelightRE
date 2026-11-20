/*
 * mcufw_ota.c -- Safe Self-OTA engine for passive WMPT slot (Phase 4b.3).
 */
#include "mcufw_ota.h"
#include "mcufw_crypto.h"
#include "mcufw_rollback.h"
#include "mcufw_rollback_hal.h"

#define MRVL_MAGIC0        0x4Du /* 'M' */
#define MRVL_MAGIC1        0x52u /* 'R' */
#define MRVL_MAGIC2        0x56u /* 'V' */
#define MRVL_MAGIC3        0x4Cu /* 'L' */
#define MRVL_MAGIC2_REF    0x2E9CF17Bu
#define OTA_CHUNK_SZ       512u
#define ROLLBACK_DELAY     3u

static const mcufw_ota_hal_t *g_hal;
static mcufw_ota_state_t g_ota;
static char g_queued_url[MCUFW_OTA_URL_MAX];
static char g_queued_md5[MCUFW_OTA_MD5_HEX + 1u];
static int  g_rollback_ticks;

static void scpy(char *dst, const char *src, size_t cap)
{
	size_t i = 0u;
	if (!dst || cap == 0u) return;
	if (src) {
		while (src[i] && i + 1u < cap) {
			dst[i] = src[i];
			i++;
		}
	}
	dst[i] = '\0';
}

static size_t slen_n(const char *s, size_t max)
{
	size_t n = 0u;
	if (!s) return 0u;
	while (n < max && s[n]) n++;
	return n;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static int url_valid(const char *u)
{
	if (!u) return 0;
	if (u[0] != 'h' || u[1] != 't' || u[2] != 't' || u[3] != 'p' ||
	    u[4] != ':' || u[5] != '/' || u[6] != '/')
		return 0;
	if (u[7] == '\0' || u[7] == '/') return 0;
	return slen_n(u, MCUFW_OTA_URL_MAX) < MCUFW_OTA_URL_MAX;
}

static int md5_hex_valid(const char *m)
{
	size_t len;
	if (!m || m[0] == '\0') return 1; /* Optional */
	len = slen_n(m, MCUFW_OTA_MD5_HEX + 2u);
	if (len != MCUFW_OTA_MD5_HEX) return 0;
	for (size_t i = 0; i < MCUFW_OTA_MD5_HEX; i++) {
		if (hex_val(m[i]) < 0) return 0;
	}
	return 1;
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int mrvl_hdr_valid(const uint8_t *hdr, size_t len)
{
	uint32_t m2, seg_cnt;
	if (len < 20u) return 0;
	if (hdr[0] != MRVL_MAGIC0 || hdr[1] != MRVL_MAGIC1 ||
	    hdr[2] != MRVL_MAGIC2 || hdr[3] != MRVL_MAGIC3)
		return 0;
	m2 = rd_le32(hdr + 4);
	seg_cnt = rd_le32(hdr + 12);
	if (m2 != MRVL_MAGIC2_REF) return 0;
	if (seg_cnt < 1u || seg_cnt > 8u) return 0;
	return 1;
}

static int set_err(const char *err)
{
	g_ota.status = MCUFW_OTA_ERR;
	scpy(g_ota.last_error, err, sizeof(g_ota.last_error));
	return -1;
}

void mcufw_ota_set_hal(const mcufw_ota_hal_t *hal)
{
	g_hal = hal;
}

void mcufw_ota_reset(void)
{
	for (size_t i = 0; i < sizeof(g_ota); i++) ((uint8_t *)&g_ota)[i] = 0u;
	g_queued_url[0] = '\0';
	g_queued_md5[0] = '\0';
	g_rollback_ticks = 0;
}

int mcufw_ota_queue(const char *url, const char *md5_hex)
{
	if (!url_valid(url) || !md5_hex_valid(md5_hex))
		return set_err("bad-args");
	if (g_ota.status == MCUFW_OTA_RUNNING || g_ota.status == MCUFW_OTA_QUEUED)
		return -1;
	scpy(g_queued_url, url, sizeof(g_queued_url));
	scpy(g_queued_md5, md5_hex ? md5_hex : "", sizeof(g_queued_md5));
	scpy(g_ota.last_url, url, sizeof(g_ota.last_url));
	g_ota.last_error[0] = '\0';
	g_ota.bytes_written = 0u;
	g_ota.bytes_total = 0u;
	g_ota.status = MCUFW_OTA_QUEUED;
	return 0;
}

void mcufw_ota_request_rollback(void)
{
	if (!g_ota.rollback_pending) {
		g_ota.rollback_pending = 1;
		g_rollback_ticks = ROLLBACK_DELAY;
	}
}

static int fail_abort(const mcufw_ota_hal_t *h, int http_open, int slot_open, const char *err)
{
	if (slot_open && h->slot_abort) h->slot_abort();
	if (http_open && h->close) h->close();
	return set_err(err);
}

int mcufw_ota_execute(const char *url, const char *md5_hex)
{
	const mcufw_ota_hal_t *h = g_hal ? g_hal : mcufw_ota_hal_default();
	uint8_t buf[OTA_CHUNK_SZ];
	uint8_t digest[MCUFW_MD5_LEN];
	mcufw_md5_ctx_t md5;
	int status = 0;
	uint32_t clen = 0u, slot_max = 0u, written = 0u;
	uint8_t chunked = 0u;
	int first_n;

	if (!h || !h->open || !h->read || !h->close ||
	    !h->slot_begin || !h->slot_write || !h->slot_complete)
		return set_err("no-hal");
	if (!url_valid(url)) return set_err("bad-url");
	if (!md5_hex_valid(md5_hex)) return set_err("bad-md5-fmt");

	scpy(g_ota.last_url, url, sizeof(g_ota.last_url));
	g_ota.last_error[0] = '\0';
	g_ota.bytes_written = 0u;
	g_ota.bytes_total = 0u;
	g_ota.status = MCUFW_OTA_RUNNING;

	if (h->open(url, &status, &clen, &chunked) != 0)
		return set_err("http-fail");
	if (status != 200)
		return fail_abort(h, 1, 0, "http-status");
	if (chunked || clen < 20u)
		return fail_abort(h, 1, 0, "bad-length");

	/* Read & validate MRVL header BEFORE opening/erasing the passive slot! */
	first_n = h->read(buf, sizeof(buf));
	if (first_n < 20)
		return fail_abort(h, 1, 0, "bad-length");
	if (!mrvl_hdr_valid(buf, (size_t)first_n))
		return fail_abort(h, 1, 0, "bad-magic");

	if (h->slot_begin(&slot_max) != 0 || slot_max < 0x10000u)
		return fail_abort(h, 1, 0, "no-passive-slot");
	if (clen > slot_max)
		return fail_abort(h, 1, 1, "oversize");

	g_ota.bytes_total = clen;
	mcufw_md5_init(&md5);

	/* Write validated first chunk + stream remainder */
	mcufw_md5_update(&md5, buf, (size_t)first_n);
	if (h->slot_write(buf, (size_t)first_n) != 0)
		return fail_abort(h, 1, 1, "flash-err");
	written = (uint32_t)first_n;
	g_ota.bytes_written = written;

	while (written < clen) {
		uint32_t remain = clen - written;
		size_t want = remain < sizeof(buf) ? (size_t)remain : sizeof(buf);
		int r = h->read(buf, want);
		if (r <= 0)
			return fail_abort(h, 1, 1, "bad-length");
		if (written + (uint32_t)r > slot_max)
			return fail_abort(h, 1, 1, "oversize");
		mcufw_md5_update(&md5, buf, (size_t)r);
		if (h->slot_write(buf, (size_t)r) != 0)
			return fail_abort(h, 1, 1, "flash-err");
		written += (uint32_t)r;
		g_ota.bytes_written = written;
	}
	h->close();

	if (written != clen)
		return fail_abort(h, 0, 1, "bad-length");

	mcufw_md5_final(&md5, digest);
	if (md5_hex && md5_hex[0]) {
		for (size_t i = 0; i < MCUFW_MD5_LEN; i++) {
			int hi = hex_val(md5_hex[i * 2u]);
			int lo = hex_val(md5_hex[i * 2u + 1u]);
			if (hi < 0 || lo < 0 ||
			    digest[i] != (uint8_t)((hi << 4) | lo))
				return fail_abort(h, 0, 1, "bad-md5");
		}
	}

	if (h->slot_complete() != 0)
		return fail_abort(h, 0, 1, "complete-err");

	mcufw_rollback_confirm_good();
	g_ota.status = MCUFW_OTA_DONE;
	if (h->reboot)
		h->reboot();
	return 0;
}

int mcufw_ota_service(void)
{
	if (g_ota.rollback_pending) {
		if (g_rollback_ticks > 0) {
			g_rollback_ticks--;
			return 0;
		}
		g_ota.rollback_pending = 0;
		if (mcufw_rollback_hal_activate_passive_slot() == 0)
			mcufw_rollback_hal_reset();
		return -1;
	}
	if (g_ota.status == MCUFW_OTA_QUEUED) {
		char url[MCUFW_OTA_URL_MAX];
		char md5[MCUFW_OTA_MD5_HEX + 1u];
		scpy(url, g_queued_url, sizeof(url));
		scpy(md5, g_queued_md5, sizeof(md5));
		g_queued_url[0] = '\0';
		g_queued_md5[0] = '\0';
		(void)mcufw_ota_execute(url, md5);
		return 1;
	}
	return 0;
}

const mcufw_ota_state_t *mcufw_ota_state(void)
{
	return &g_ota;
}

const char *mcufw_ota_status_name(mcufw_ota_status_t s)
{
	switch (s) {
	case MCUFW_OTA_IDLE:    return "idle";
	case MCUFW_OTA_QUEUED:  return "queued";
	case MCUFW_OTA_RUNNING: return "running";
	case MCUFW_OTA_DONE:    return "done";
	case MCUFW_OTA_ERR:     return "error";
	default:                return "?";
	}
}
