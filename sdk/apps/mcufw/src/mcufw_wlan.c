/*
 * mcufw_wlan.c -- mlan/mdev link boundary + bring-up state machine.
 *
 * Why "address-only" references instead of calls:
 *   `wlan_init_adapter()` / `wlan_init_fw()` take a pointer to the mlan
 *   adapter that the SDK allocates for itself; `CLK_Init()` sits in front of
 *   `os_init()`/`vTaskStartScheduler()`.  Calling either from a bare-metal
 *   main() would run half an SDK init against uninitialised structures.
 *   Taking the *address* pulls the archive members into the image (that is
 *   everything the linker needs, and everything phase 3.4 has to prove) while
 *   the execution order stays documented and unused until the scheduler and
 *   the lwIP core exist (phase 5).
 *
 * Proof of the members: `arm-none-eabi-nm mcufw.elf` shows
 *   T wlan_init_lock_list / wlan_init_adapter / wlan_init_fw ...  (mlan_init.o)
 *   T CLK_Init                                                  (mdev_startup.o)
 * and mcufw.map carries `libwmsdk.a(mlan_init.o)` / `(mdev_startup.o)`.
 */
#include "mcufw_wlan.h"

/* --- vendor entry points (libwmsdk.a) ------------------------------------ *
 * Declared as byte arrays: the real prototypes live behind the vendor include
 * chain (mlan.h/mdev.h -> mw300.h -> ...), and a wrong prototype would invite
 * a call that this phase must not make.  Taking the address is ABI-neutral. */
extern const unsigned char wlan_init_lock_list[];   /* mlan_init.o     */
extern const unsigned char wlan_init_adapter[];     /* mlan_init.o     */
extern const unsigned char wlan_init_priv[];        /* mlan_init.o     */
extern const unsigned char wlan_init_fw[];          /* mlan_init.o     */
extern const unsigned char wlan_allocate_adapter[]; /* mlan_init.o     */
extern const unsigned char CLK_Init[];              /* mdev_startup.o  */
extern const unsigned char boot_init[];             /* mdev_startup.o  */

/* RTOS/lwIP boundary: 0 while the kernel is a link shim (phase 3.4). */
extern int mcufw_os_ready(void);

/* NVRAM-resident configuration (0x480c0000, survives a reboot; the container
 * does not carry it -- it is NOBITS in mw300_min.ld, and its content is
 * whatever the previous boot / the factory left there, hence the magic
 * check). */
__attribute__((section(".nvram"), used))
static mcufw_wlan_cfg_t nvram_cfg;

static mcufw_wlan_cfg_t fallback_cfg;

static const mcufw_wlan_anchor_t anchors[] = {
	{ "wlan_init_lock_list",   "libwmsdk.a(mlan_init.o)",    wlan_init_lock_list },
	{ "wlan_init_adapter",     "libwmsdk.a(mlan_init.o)",    wlan_init_adapter },
	{ "wlan_init_priv",        "libwmsdk.a(mlan_init.o)",    wlan_init_priv },
	{ "wlan_init_fw",          "libwmsdk.a(mlan_init.o)",    wlan_init_fw },
	{ "wlan_allocate_adapter", "libwmsdk.a(mlan_init.o)",    wlan_allocate_adapter },
	{ "CLK_Init",              "libwmsdk.a(mdev_startup.o)", CLK_Init },
	{ "boot_init",             "libwmsdk.a(mdev_startup.o)", boot_init },
};

static unsigned slen(const char *s)
{
	unsigned n = 0;

	while (s[n])
		n++;
	return n;
}

static void scpy(char *dst, const char *src, unsigned cap)
{
	unsigned n = 0;

	while (src[n] && n + 1u < cap) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
}

const mcufw_wlan_cfg_t *mcufw_wlan_default_cfg(void)
{
	scpy(fallback_cfg.ssid, MCUFW_WLAN_DEFAULT_SSID, sizeof(fallback_cfg.ssid));
	scpy(fallback_cfg.psk, MCUFW_WLAN_DEFAULT_PSK, sizeof(fallback_cfg.psk));
	fallback_cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
	fallback_cfg.muid = MCUFW_WLAN_DEFAULT_MUID;
	fallback_cfg.provisioned =
		(uint8_t)(MCUFW_WLAN_DEFAULT_SSID[0] ? 1u : 0u);
	fallback_cfg.reserved[0] = 0;
	fallback_cfg.reserved[1] = 0;
	fallback_cfg.reserved[2] = 0;
	return &fallback_cfg;
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
	unsigned ssid_len;
	unsigned psk_len;

	if (!cfg)
		return MCUFW_WLAN_E_NVRAM;

	ssid_len = slen(cfg->ssid);
	if (ssid_len < 1u || ssid_len > MCUFW_WLAN_SSID_MAX)
		return MCUFW_WLAN_E_SSID;

	psk_len = slen(cfg->psk);
	/* WPA2-PSK: 8..63 ASCII, or 64 hex chars (= 32 raw bytes). */
	if (psk_len != 64u && (psk_len < 8u || psk_len > MCUFW_WLAN_PSK_MAX))
		return MCUFW_WLAN_E_PSK;

	return MCUFW_WLAN_OK;
}

unsigned mcufw_wlan_link_proof(mcufw_wlan_anchor_t *out, unsigned max)
{
	unsigned n = 0;

	for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
		if (!out || n >= max)
			break;
		out[n++] = anchors[i];
	}
	return (unsigned)(sizeof(anchors) / sizeof(anchors[0]));
}

int mcufw_wlan_bringup(const mcufw_wlan_cfg_t *cfg, mcufw_wlan_report_t *rep)
{
	int rc;

	rep->cfg_valid = 0;
	rep->anchors = (uint8_t)(sizeof(anchors) / sizeof(anchors[0]));
	rep->stage = 1;                       /* 1: cfg validated              */
	rep->reserved = 0;

	rc = mcufw_wlan_cfg_validate(cfg);
	rep->rc = rc;
	if (rc != MCUFW_WLAN_OK)
		return rc;
	rep->cfg_valid = 1;

	/* 2: link boundary -- every anchor must have a real address; a zero
	 * would mean the archive member was not pulled. */
	rep->stage = 2;
	for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
		if (!anchors[i].addr)
			return MCUFW_WLAN_E_NVRAM;
	}

	/* 3: mlan/mdev init.  Needs os_init() == real kernel.  With the link
	 * shim in place this stage reports E_NEEDS_RTOS instead of pretending
	 * to run mlan_init_fw() against uninitialised SDK state. */
	rep->stage = 3;
	if (!mcufw_os_ready())
		return MCUFW_WLAN_E_NEEDS_RTOS;

	/* 4 (phase 5): mlan_init_fw() -> wm_core_init() -> net_wlan_init()
	 *              -> wm_wlan_connect(cfg->ssid, cfg->psk) */
	rep->stage = 4;
	return MCUFW_WLAN_OK;
}

const char *mcufw_wlan_strerror(int rc)
{
	switch (rc) {
	case MCUFW_WLAN_OK:          return "ok";
	case MCUFW_WLAN_E_SSID:      return "bad-ssid";
	case MCUFW_WLAN_E_PSK:       return "bad-psk";
	case MCUFW_WLAN_E_NVRAM:     return "bad-nvram";
	case MCUFW_WLAN_E_NEEDS_RTOS: return "needs-rtos";
	default:                     return "?";
	}
}

const char *mcufw_wlan_stage_name(unsigned stage)
{
	switch (stage) {
	case 1:  return "cfg";
	case 2:  return "link";
	case 3:  return "mlan";
	case 4:  return "assoc";
	default: return "?";
	}
}
