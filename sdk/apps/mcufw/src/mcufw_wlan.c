/*
 * mcufw_wlan.c -- mlan/mdev bring-up with real calls via HAL seam.
 *
 * Phase 4b.2: Replaces address-only anchors with actual wlan_init() +
 * wm_wlan_connect() calls through a swappable HAL layer. Unprovisioned
 * devices fall back to a Setup-AP (yeelink-light-<did>).
 */
#include "mcufw_wlan.h"

/* --- vendor entry points (libwmsdk.a) ------------------------------------ */
#ifndef HOST_TEST
extern const unsigned char wlan_init_lock_list[];
extern const unsigned char wlan_init_adapter[];
extern const unsigned char wlan_init_priv[];
extern const unsigned char wlan_init_fw[];
extern const unsigned char wlan_allocate_adapter[];
extern const unsigned char CLK_Init[];
extern const unsigned char boot_init[];
#else
static const unsigned char wlan_init_lock_list[1] = {0};
static const unsigned char wlan_init_adapter[1] = {0};
static const unsigned char wlan_init_priv[1] = {0};
static const unsigned char wlan_init_fw[1] = {0};
static const unsigned char wlan_allocate_adapter[1] = {0};
static const unsigned char CLK_Init[1] = {0};
static const unsigned char boot_init[1] = {0};
#endif

#ifndef HOST_TEST
extern int mcufw_os_ready(void);
/* Real WMSDK prototypes from libwmsdk.a (wm_wlan_init reads WMPT wififw) */
extern int wm_core_init(void);
extern int wm_wlan_init(void);
extern int wm_wlan_connect(char *ssid, char *psk);
extern int app_uap_start_with_dhcp(const char *ssid, const char *psk);
extern void wm_wlan_deinit(void);
#endif

/* --- NVRAM config -------------------------------------------------------- */
#ifndef HOST_TEST
__attribute__((section(".nvram"), used))
static mcufw_wlan_cfg_t nvram_cfg;
#else
static mcufw_wlan_cfg_t nvram_cfg;
#endif

static mcufw_wlan_cfg_t fallback_cfg;
static mcufw_wlan_state_t current_state = MCUFW_WLAN_STATE_INIT;

__attribute__((used))
static const mcufw_wlan_anchor_t anchors[] = {
	{ "wlan_init_lock_list",   "libwmsdk.a(mlan_init.o)",    wlan_init_lock_list },
	{ "wlan_init_adapter",     "libwmsdk.a(mlan_init.o)",    wlan_init_adapter },
	{ "wlan_init_priv",        "libwmsdk.a(mlan_init.o)",    wlan_init_priv },
	{ "wlan_init_fw",          "libwmsdk.a(mlan_init.o)",    wlan_init_fw },
	{ "wlan_allocate_adapter", "libwmsdk.a(mlan_init.o)",    wlan_allocate_adapter },
	{ "CLK_Init",              "libwmsdk.a(mdev_startup.o)", CLK_Init },
	{ "boot_init",             "libwmsdk.a(mdev_startup.o)", boot_init },
};

/* --- Real WMSDK HAL implementation --------------------------------------- */
#ifndef HOST_TEST
static int real_wlan_init(void) {
	/* wm_core_init() is idempotent and prepares wmtime/pm/healthmon/part_init
	 * needed by wm_wlan_init() to locate WMPT wififw partition. */
	(void)wm_core_init();
	return wm_wlan_init();
}
static int real_wlan_connect(char *ssid, char *psk) { return wm_wlan_connect(ssid, psk); }
static int real_uap_start(const char *ssid, const char *psk) { return app_uap_start_with_dhcp(ssid, psk); }
static void real_wlan_deinit(void) { wm_wlan_deinit(); }

static const mcufw_wlan_hal_t real_hal = {
	real_wlan_init, real_wlan_connect, real_uap_start, real_wlan_deinit
};
#endif

static const mcufw_wlan_hal_t *active_hal = NULL;

void mcufw_wlan_set_hal(const mcufw_wlan_hal_t *hal)
{
	active_hal = hal;
}

const mcufw_wlan_hal_t *mcufw_wlan_get_hal(void)
{
#ifndef HOST_TEST
	if (!active_hal)
		return &real_hal;
#endif
	return active_hal;
}

/* --- Helpers ------------------------------------------------------------- */
static unsigned slen(const char *s)
{
	unsigned n = 0;
	while (s[n]) n++;
	return n;
}

static void scpy(char *dst, const char *src, unsigned cap)
{
	unsigned n = 0;
	while (src[n] && n + 1u < cap) { dst[n] = src[n]; n++; }
	dst[n] = 0;
}

int mcufw_wlan_setup_ap_ssid(char *buf, size_t cap, uint32_t did)
{
	const char prefix[] = "yeelink-light-";
	unsigned i = 0;
	for (; prefix[i] && i < cap - 1; i++)
		buf[i] = prefix[i];
	/* Append hex DID */
	for (int shift = 28; shift >= 0 && i < cap - 1; shift -= 4)
		buf[i++] = "0123456789abcdef"[(did >> shift) & 0xf];
	buf[i] = 0;
	return (int)i;
}

/* --- Config API ---------------------------------------------------------- */
const mcufw_wlan_cfg_t *mcufw_wlan_default_cfg(void)
{
	scpy(fallback_cfg.ssid, MCUFW_WLAN_DEFAULT_SSID, sizeof(fallback_cfg.ssid));
	scpy(fallback_cfg.psk, MCUFW_WLAN_DEFAULT_PSK, sizeof(fallback_cfg.psk));
	fallback_cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
	fallback_cfg.muid = MCUFW_WLAN_DEFAULT_MUID;
	fallback_cfg.provisioned = (uint8_t)(MCUFW_WLAN_DEFAULT_SSID[0] ? 1u : 0u);
	fallback_cfg.reserved[0] = 0;
	fallback_cfg.reserved[1] = 0;
	fallback_cfg.reserved[2] = 0;
	return &fallback_cfg;
}

int mcufw_wlan_set_cfg(const char *ssid, const char *psk, uint8_t provisioned)
{
	mcufw_wlan_cfg_t tmp;

	scpy(tmp.ssid, ssid ? ssid : "", sizeof(tmp.ssid));
	scpy(tmp.psk, psk ? psk : "", sizeof(tmp.psk));
	tmp.magic = MCUFW_WLAN_NVRAM_MAGIC;
	tmp.muid = MCUFW_WLAN_DID;
	tmp.provisioned = provisioned ? 1u : 0u;
	tmp.reserved[0] = 0u;
	tmp.reserved[1] = 0u;
	tmp.reserved[2] = 0u;
	if (tmp.provisioned && mcufw_wlan_cfg_validate(&tmp) != MCUFW_WLAN_OK)
		return -1;
	nvram_cfg = tmp;
	return 0;
}

const mcufw_wlan_cfg_t *mcufw_wlan_cfg(void)
{
	const mcufw_wlan_cfg_t *n = &nvram_cfg;
	if (n->magic == MCUFW_WLAN_NVRAM_MAGIC)
		return n;
	return mcufw_wlan_default_cfg();
}

int mcufw_wlan_cfg_validate(const mcufw_wlan_cfg_t *cfg)
{
	unsigned ssid_len, psk_len;
	if (!cfg) return MCUFW_WLAN_E_NVRAM;
	ssid_len = slen(cfg->ssid);
	if (ssid_len < 1u || ssid_len > MCUFW_WLAN_SSID_MAX)
		return MCUFW_WLAN_E_SSID;
	psk_len = slen(cfg->psk);
	if (psk_len != 64u && (psk_len < 8u || psk_len > MCUFW_WLAN_PSK_MAX))
		return MCUFW_WLAN_E_PSK;
	return MCUFW_WLAN_OK;
}

unsigned mcufw_wlan_link_proof(mcufw_wlan_anchor_t *out, unsigned max)
{
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
		if (!out || n >= max) break;
		out[n++] = anchors[i];
	}
	return (unsigned)(sizeof(anchors) / sizeof(anchors[0]));
}

/* --- State machine ------------------------------------------------------- */
mcufw_wlan_state_t mcufw_wlan_get_state(void)
{
	return current_state;
}

int mcufw_wlan_bringup(const mcufw_wlan_cfg_t *cfg, mcufw_wlan_report_t *rep)
{
	const mcufw_wlan_hal_t *hal = mcufw_wlan_get_hal();
	int rc;

	rep->cfg_valid = 0;
	rep->anchors = (uint8_t)(sizeof(anchors) / sizeof(anchors[0]));
	rep->stage = 1;
	rep->reserved = 0;

	/* Stage 1: Validate config (skip SSID/PSK check for unprovisioned -> AP fallback) */
	if (!cfg) {
		current_state = MCUFW_WLAN_STATE_ERROR;
		rep->rc = MCUFW_WLAN_E_NVRAM;
		return MCUFW_WLAN_E_NVRAM;
	}
	if (cfg->provisioned) {
		rc = mcufw_wlan_cfg_validate(cfg);
		rep->rc = rc;
		if (rc != MCUFW_WLAN_OK) {
			current_state = MCUFW_WLAN_STATE_ERROR;
			return rc;
		}
		rep->cfg_valid = 1;
	} else {
		/* Unprovisioned: no STA credentials needed, AP fallback is valid */
		rep->cfg_valid = 1;
	}

	/* Stage 2: Link proof */
	rep->stage = 2;
	for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
		if (!anchors[i].addr) {
			current_state = MCUFW_WLAN_STATE_ERROR;
			return MCUFW_WLAN_E_NVRAM;
		}
	}

	/* Stage 3: RTOS check (target only) */
	rep->stage = 3;
#ifndef HOST_TEST
	if (!mcufw_os_ready()) {
		current_state = MCUFW_WLAN_STATE_ERROR;
		return MCUFW_WLAN_E_NEEDS_RTOS;
	}
#endif

	/* Stage 4: No HAL installed -> error */
	rep->stage = 4;
	if (!hal || !hal->wlan_init || !hal->wlan_connect) {
		current_state = MCUFW_WLAN_STATE_ERROR;
		return MCUFW_WLAN_E_ASSOC_FAIL;
	}

	/* Stage 5: wlan_init() */
	rep->stage = 5;
	rc = hal->wlan_init();
	if (rc != 0) {
		current_state = MCUFW_WLAN_STATE_ERROR;
		rep->rc = MCUFW_WLAN_E_ASSOC_FAIL;
		return MCUFW_WLAN_E_ASSOC_FAIL;
	}

	/* Stage 6: STA join or AP fallback */
	rep->stage = 6;
	if (cfg->provisioned) {
		current_state = MCUFW_WLAN_STATE_STA_JOINING;
		/* Cast away const for WMSDK API compatibility */
		rc = hal->wlan_connect((char *)cfg->ssid, (char *)cfg->psk);
		if (rc == 0) {
			current_state = MCUFW_WLAN_STATE_STA_CONNECTED;
		} else {
			/* Join failed -> AP fallback */
			char ap_ssid[48];
			mcufw_wlan_setup_ap_ssid(ap_ssid, sizeof(ap_ssid), MCUFW_WLAN_DID);
			current_state = MCUFW_WLAN_STATE_AP_FALLBACK;
			if (hal->uap_start) {
				rc = hal->uap_start(ap_ssid, "");
				if (rc != 0) {
					current_state = MCUFW_WLAN_STATE_ERROR;
					rep->rc = MCUFW_WLAN_E_ASSOC_FAIL;
					return MCUFW_WLAN_E_ASSOC_FAIL;
				}
			}
		}
	} else {
		/* Unprovisioned -> Setup-AP */
		char ap_ssid[48];
		mcufw_wlan_setup_ap_ssid(ap_ssid, sizeof(ap_ssid), MCUFW_WLAN_DID);
		current_state = MCUFW_WLAN_STATE_AP_FALLBACK;
		if (hal->uap_start) {
			rc = hal->uap_start(ap_ssid, "");
			if (rc != 0) {
				current_state = MCUFW_WLAN_STATE_ERROR;
				rep->rc = MCUFW_WLAN_E_ASSOC_FAIL;
				return MCUFW_WLAN_E_ASSOC_FAIL;
			}
		}
	}

	rep->rc = MCUFW_WLAN_OK;
	return MCUFW_WLAN_OK;
}

/* --- String helpers ------------------------------------------------------ */
const char *mcufw_wlan_strerror(int rc)
{
	switch (rc) {
	case MCUFW_WLAN_OK:            return "ok";
	case MCUFW_WLAN_E_SSID:        return "bad-ssid";
	case MCUFW_WLAN_E_PSK:         return "bad-psk";
	case MCUFW_WLAN_E_NVRAM:       return "bad-nvram";
	case MCUFW_WLAN_E_NEEDS_RTOS:  return "needs-rtos";
	case MCUFW_WLAN_E_ASSOC_FAIL:  return "assoc-fail";
	default:                       return "?";
	}
}

const char *mcufw_wlan_stage_name(unsigned stage)
{
	switch (stage) {
	case 1:  return "cfg";
	case 2:  return "link";
	case 3:  return "rtos";
	case 4:  return "hal";
	case 5:  return "init";
	case 6:  return "assoc";
	default: return "?";
	}
}

const char *mcufw_wlan_state_name(mcufw_wlan_state_t s)
{
	switch (s) {
	case MCUFW_WLAN_STATE_INIT:         return "init";
	case MCUFW_WLAN_STATE_STA_JOINING:  return "sta_joining";
	case MCUFW_WLAN_STATE_STA_CONNECTED:return "sta_connected";
	case MCUFW_WLAN_STATE_AP_FALLBACK:  return "ap_fallback";
	case MCUFW_WLAN_STATE_ERROR:        return "error";
	default:                            return "?";
	}
}

#ifdef HOST_TEST
void mcufw_wlan_test_reset(void)
{
	current_state = MCUFW_WLAN_STATE_INIT;
	active_hal = NULL;
	nvram_cfg.magic = 0;
	nvram_cfg.provisioned = 0;
}

void mcufw_wlan_test_set_provisioned(int prov)
{
	nvram_cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
	nvram_cfg.provisioned = (uint8_t)(prov ? 1 : 0);
	if (prov) {
		scpy(nvram_cfg.ssid, "TestSSID", sizeof(nvram_cfg.ssid));
		scpy(nvram_cfg.psk, "TestPassword123", sizeof(nvram_cfg.psk));
	}
}
#endif
