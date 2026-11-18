/*
 * mcufw_config.h -- persistent lamp configuration (phase 4).
 *
 * One small CRC-protected block holds what the lamp must remember across a
 * power cycle: power state, last brightness / colour temperature / HSV and
 * the device name.  The boot path loads it BEFORE the first
 * mcufw_state_apply(), so the Yeelight power-on promise holds:
 *
 *   lamp was on  -> comes back on with the last brightness / colour
 *   lamp was off -> stays off (all four channels dark)
 *
 * Storage goes through the link-time seam mcufw_config_hal_{read,write,name}
 * exactly like mcufw_pwm does for GPT0 -- no #ifdef in any caller:
 *
 *   src/mcufw_config_hal_host.c        byte array + write counter (host test)
 *   src/mcufw_config_hal_target_stub.c DECLARED STUB, no persistence (default)
 *   src/mcufw_config_hal_target_flash.c real flash_drv_* backend, opt-in
 *
 * MCUFW_CONFIG_BACKEND selects the target backend at build time
 * (stub|flash, default stub).  The stub is deliberately honest: its name says
 * "stub-no-persist", reads return -1 so every boot falls back to defaults,
 * and writes are counted but discarded.  Nothing silently pretends that the
 * lamp already remembers anything.
 *
 * Robustness: bad magic, wrong version, wrong size or a CRC mismatch all lead
 * to the defined defaults -- never a brick, never a retry loop -- and produce
 * exactly ONE log line per load attempt.
 *
 * Write discipline: saving happens only on an explicit save call or when the
 * runtime loop observes a NEW state mutation.  An idle tick performs zero
 * config writes (asserted by the host-test section `config_persist`).
 */
#ifndef MCUFW_CONFIG_H
#define MCUFW_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "mcufw_state.h"

/* 'M','C','F','W' little endian on the medium. */
#define MCUFW_CONFIG_MAGIC    0x5746434du
#define MCUFW_CONFIG_VERSION  1u

/* Serialised block, 48 bytes total (SRAM1 is tight -- keep it small):
 *
 *   off  size  field
 *     0    4   magic
 *     4    2   version
 *     6    1   power       (0/1)
 *     7    1   bright      (1..100)
 *     8    2   ct          (1700..6500)
 *    10    2   hue         (0..359)
 *    12    1   sat         (0..100)
 *    13    1   color_mode  (2 = ct, 3 = hsv)
 *    14   30   name        NUL-terminated, sanitised like mcufw_state
 *    44    4   crc32       over bytes 0..43
 *
 * The name limit here is the PERSIST limit; mcufw_state still allows 32
 * characters at runtime, a longer name is truncated on save (documented).
 */
#define MCUFW_CONFIG_NAME_MAX 30u
#define MCUFW_CONFIG_SIZE     48u
#define MCUFW_CONFIG_CRC_OFF  44u

/* Load status codes (also the reason for the single log line). */
#define MCUFW_CFG_OK           0
#define MCUFW_CFG_ERR_READ    (-1)  /* backend has no (readable) image      */
#define MCUFW_CFG_ERR_MAGIC   (-2)
#define MCUFW_CFG_ERR_VERSION (-3)
#define MCUFW_CFG_ERR_CRC     (-4)
#define MCUFW_CFG_ERR_WRITE   (-5)
#define MCUFW_CFG_ERR_ARG     (-6)

typedef struct {
	uint32_t loads;          /* mcufw_config_load() calls                */
	uint32_t load_ok;        /* blocks accepted verbatim                 */
	uint32_t load_defaults;  /* loads that fell back to defaults         */
	uint32_t saves;          /* mcufw_config_save() calls                */
	uint32_t save_ok;        /* successful backend writes                */
	uint32_t save_failed;    /* backend refused the write                */
	uint32_t save_skipped;   /* identical block, nothing written         */
	uint32_t hal_writes;     /* mcufw_config_hal_write() invocations     */
	uint32_t foreign_fields; /* fields taken from a REJECTED block (0!)  */
	uint32_t logs;           /* log lines emitted (1 per failed load)    */
	int      last_status;    /* last load status code                    */
	char     last_log[80];   /* last log line (UART mirror)              */
} mcufw_config_stats_t;

/* Reset counters + cached block (does NOT touch the storage backend). */
void mcufw_config_init(void);

/* Read + validate the stored block into `st`.
 * On any failure `st` is left exactly as mcufw_state_init() defines it and
 * exactly one log line is emitted.  Returns MCUFW_CFG_* . */
int mcufw_config_load(mcufw_state_t *st);

/* Serialise `st` and write it through the HAL seam.  A block identical to the
 * last written one is skipped (save_skipped++), so repeated saves of an
 * unchanged lamp do not wear the flash. */
int mcufw_config_save(const mcufw_state_t *st);

/* Boot helper: load the config, install it into `st` WITHOUT applying it to
 * the PWM backend (the caller decides when to apply), reset the mutation
 * counter to 0 and arm the runtime auto-save baseline.
 * Returns the load status. */
int mcufw_config_boot(mcufw_state_t *st);

/* Called once per runtime tick AFTER the network pumps: saves only if the
 * state mutation counter changed since the last save/load.  Returns 1 if a
 * write happened, 0 otherwise (idle). */
int mcufw_config_service(mcufw_state_t *st);

const mcufw_config_stats_t *mcufw_config_stats(void);

/* Reflected CRC-32 (poly 0xEDB88320), init 0, NO final xor -- the same variant
 * the MRVL container uses (sdk/layout.txt).  zlib.crc32() would differ. */
uint32_t mcufw_config_crc32(const uint8_t *buf, size_t len);

/* Serialise without touching the backend (exposed for the host test). */
size_t mcufw_config_encode(const mcufw_state_t *st, uint8_t *out, size_t cap);

#endif /* MCUFW_CONFIG_H */
