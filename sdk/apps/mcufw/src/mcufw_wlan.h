/*
 * mcufw_wlan.h -- WLAN bring-up layer for mcufw (88MW300 / libwmsdk mlan).
 *
 * Phase 4b.2: Real wlan_init() + wm_wlan_connect() calls via a HAL seam,
 * Setup-AP fallback when unprovisioned, deterministic state machine.
 */
#ifndef MCUFW_WLAN_H
#define MCUFW_WLAN_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_WLAN_SSID_MAX     32u
#define MCUFW_WLAN_PSK_MAX      63u
#define MCUFW_WLAN_NVRAM_MAGIC  0x4d435746u   /* "MCFG" */
#define MCUFW_WLAN_DID          0x05eb8034u

#ifndef MCUFW_WLAN_DEFAULT_SSID
#define MCUFW_WLAN_DEFAULT_SSID ""
#endif
#ifndef MCUFW_WLAN_DEFAULT_PSK
#define MCUFW_WLAN_DEFAULT_PSK  ""
#endif
#ifndef MCUFW_WLAN_DEFAULT_MUID
#define MCUFW_WLAN_DEFAULT_MUID 0u
#endif

typedef struct {
	uint32_t magic;
	uint32_t muid;
	char     ssid[MCUFW_WLAN_SSID_MAX + 1u];
	char     psk[MCUFW_WLAN_PSK_MAX + 1u];
	uint8_t  provisioned;
	uint8_t  reserved[3];
} mcufw_wlan_cfg_t;

enum {
	MCUFW_WLAN_OK = 0,
	MCUFW_WLAN_E_SSID = 1,
	MCUFW_WLAN_E_PSK = 2,
	MCUFW_WLAN_E_NVRAM = 3,
	MCUFW_WLAN_E_NEEDS_RTOS = 4,
	MCUFW_WLAN_E_ASSOC_FAIL = 5,
};

/* State machine phases */
typedef enum {
	MCUFW_WLAN_STATE_INIT = 0,
	MCUFW_WLAN_STATE_STA_JOINING,
	MCUFW_WLAN_STATE_STA_CONNECTED,
	MCUFW_WLAN_STATE_AP_FALLBACK,
	MCUFW_WLAN_STATE_ERROR,
} mcufw_wlan_state_t;

typedef struct {
	uint8_t  cfg_valid;
	uint8_t  anchors;
	uint8_t  stage;
	uint8_t  reserved;
	int      rc;
} mcufw_wlan_report_t;

typedef struct {
	const char *name;
	const char *member;
	const void *addr;
} mcufw_wlan_anchor_t;

/* --- HAL seam for host-testability --------------------------------------- */
typedef struct {
	int  (*wlan_init)(void);
	int  (*wlan_connect)(char *ssid, char *psk);
	int  (*uap_start)(const char *ssid, const char *psk);
	void (*wlan_deinit)(void);
} mcufw_wlan_hal_t;

/* Install a custom HAL (host-test stub). NULL restores the real WMSDK HAL. */
void mcufw_wlan_set_hal(const mcufw_wlan_hal_t *hal);
const mcufw_wlan_hal_t *mcufw_wlan_get_hal(void);

/* --- Public API ---------------------------------------------------------- */
const mcufw_wlan_cfg_t *mcufw_wlan_default_cfg(void);
const mcufw_wlan_cfg_t *mcufw_wlan_cfg(void);
int mcufw_wlan_cfg_validate(const mcufw_wlan_cfg_t *cfg);
unsigned mcufw_wlan_link_proof(mcufw_wlan_anchor_t *out, unsigned max);

/* Full bringup: validates cfg, inits WLAN, joins STA or falls back to AP.
 * Returns MCUFW_WLAN_OK on success, error code otherwise.
 * The state machine is deterministic and testable via the HAL seam. */
int mcufw_wlan_bringup(const mcufw_wlan_cfg_t *cfg, mcufw_wlan_report_t *rep);

/* Query current state machine phase */
mcufw_wlan_state_t mcufw_wlan_get_state(void);

/* Build the Setup-AP SSID: "yeelink-light-<did_hex>" */
int mcufw_wlan_setup_ap_ssid(char *buf, size_t cap, uint32_t did);

const char *mcufw_wlan_strerror(int rc);
const char *mcufw_wlan_stage_name(unsigned stage);
const char *mcufw_wlan_state_name(mcufw_wlan_state_t s);

/* Test helpers (only available under HOST_TEST) */
#ifdef HOST_TEST
void mcufw_wlan_test_reset(void);
void mcufw_wlan_test_set_provisioned(int prov);
#endif

#endif /* MCUFW_WLAN_H */
