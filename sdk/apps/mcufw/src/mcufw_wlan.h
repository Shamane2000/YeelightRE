/*
 * mcufw_wlan.h -- WLAN bring-up layer for the mcufw (88MW300 / libwmsdk mlan).
 *
 * Phase 3.4 scope: the mlan/mdev init path of libwmsdk.a is *linked* into the
 * image and its call sequence is implemented here; executing it needs the
 * scheduler (os_init -> vTaskStartScheduler) and the lwIP core, neither of
 * which is part of 3.4.  mcufw_wlan_bringup() therefore reports the stage it
 * reached instead of pretending to be associated -- the actual AP join is the
 * flash gate of phase 5 (docs/10 §4).
 *
 * Configuration comes from a NVRAM-resident struct (`.nvram` section at
 * 0x480c0000) with compile-time defaults; SSID/PSK stay configurable without
 * a rebuild, and the same struct is what the miIO provisioning handler
 * (phase 3.5) will write.
 */
#ifndef MCUFW_WLAN_H
#define MCUFW_WLAN_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_WLAN_SSID_MAX     32u
#define MCUFW_WLAN_PSK_MAX      63u
#define MCUFW_WLAN_NVRAM_MAGIC  0x4d435746u   /* "MCFG" */

/* Defaults are compile-time overridable (-DMCUFW_WLAN_DEFAULT_SSID='"lab"'). */
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
	uint32_t magic;                     /* MCUFW_WLAN_NVRAM_MAGIC         */
	uint32_t muid;                      /* Mi-account id (0 = unpaired)   */
	char     ssid[MCUFW_WLAN_SSID_MAX + 1u];
	char     psk[MCUFW_WLAN_PSK_MAX + 1u];
	uint8_t  provisioned;               /* 1 = credentials were written   */
	uint8_t  reserved[3];
} mcufw_wlan_cfg_t;

enum {
	MCUFW_WLAN_OK = 0,
	MCUFW_WLAN_E_SSID = 1,
	MCUFW_WLAN_E_PSK = 2,
	MCUFW_WLAN_E_NVRAM = 3,
	MCUFW_WLAN_E_NEEDS_RTOS = 4,
};

typedef struct {
	uint8_t  cfg_valid;                 /* NVRAM/compile-time cfg passed  */
	uint8_t  anchors;                   /* vendor symbols linked (count)   */
	uint8_t  stage;                     /* last bring-up stage reached     */
	uint8_t  reserved;
	int      rc;
} mcufw_wlan_report_t;

typedef struct {
	const char *name;                   /* vendor symbol                  */
	const char *member;                 /* archive member it comes from   */
	const void *addr;
} mcufw_wlan_anchor_t;

const mcufw_wlan_cfg_t *mcufw_wlan_default_cfg(void);

/* The NVRAM copy is authoritative when its magic matches, else defaults. */
const mcufw_wlan_cfg_t *mcufw_wlan_cfg(void);
int mcufw_wlan_cfg_validate(const mcufw_wlan_cfg_t *cfg);

/* Fills up to `max` entries of the link-proof table and returns the count.
 * This is what proves in `nm`/the map file that libwmsdk.a(mlan_init.o) and
 * libwmsdk.a(mdev_startup.o) really ended up in the image. */
unsigned mcufw_wlan_link_proof(mcufw_wlan_anchor_t *out, unsigned max);

int mcufw_wlan_bringup(const mcufw_wlan_cfg_t *cfg, mcufw_wlan_report_t *rep);

const char *mcufw_wlan_strerror(int rc);
const char *mcufw_wlan_stage_name(unsigned stage);

#endif /* MCUFW_WLAN_H */
