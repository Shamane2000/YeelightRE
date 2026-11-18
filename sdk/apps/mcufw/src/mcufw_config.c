/*
 * mcufw_config.c -- persistent lamp configuration (phase 4).
 *
 * Free of libc, RTOS and register access: the very same code runs in the host
 * test and on the target.  Storage is reached only through
 * mcufw_config_hal_{read,write} (link-time seam).
 *
 * Failure policy (no brick, no retry storm):
 *   unreadable / bad magic / wrong version / CRC mismatch
 *     -> keep the mcufw_state_init() defaults, count load_defaults++,
 *        emit EXACTLY ONE log line, take ZERO fields from the bad block
 *        (foreign_fields stays 0 -- asserted by the host test).
 *
 * Write policy:
 *   mcufw_config_save()      explicit save path
 *   mcufw_config_service()   once per runtime tick, only on a NEW mutation
 *   identical block          skipped, no backend write at all
 */
#include "mcufw_config.h"
#include "mcufw_config_hal.h"

static mcufw_config_stats_t g_cfg;
static uint8_t  g_last[MCUFW_CONFIG_SIZE];
static int      g_have_last;
static uint32_t g_saved_mutations;
static int      g_armed;

/* ------------------------------------------------------------- formatting -- */
static void str_put(char *dst, size_t cap, size_t *pos, const char *s)
{
	while (*s && *pos + 1u < cap)
		dst[(*pos)++] = *s++;
	dst[*pos] = '\0';
}

static void hex_put(char *dst, size_t cap, size_t *pos, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	str_put(dst, cap, pos, "0x");
	for (shift = 28; shift >= 0; shift -= 4) {
		if (*pos + 1u >= cap)
			break;
		dst[(*pos)++] = digits[(v >> (unsigned)shift) & 0xfu];
	}
	dst[*pos] = '\0';
}

/* One log line per rejected load -- this is the single UART hint the ticket
 * asks for; the runtime loop mirrors last_log once, it does not re-log. */
static void cfg_log(const char *reason, uint32_t detail)
{
	size_t pos = 0;

	str_put(g_cfg.last_log, sizeof(g_cfg.last_log), &pos, "config ");
	str_put(g_cfg.last_log, sizeof(g_cfg.last_log), &pos, reason);
	str_put(g_cfg.last_log, sizeof(g_cfg.last_log), &pos, " -> defaults (");
	hex_put(g_cfg.last_log, sizeof(g_cfg.last_log), &pos, detail);
	str_put(g_cfg.last_log, sizeof(g_cfg.last_log), &pos, ")");
	g_cfg.logs++;
}

/* ------------------------------------------------------------------- crc --- */
uint32_t mcufw_config_crc32(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0u;
	size_t i;
	unsigned bit;

	for (i = 0; i < len; i++) {
		crc ^= (uint32_t)buf[i];
		for (bit = 0; bit < 8u; bit++) {
			if (crc & 1u)
				crc = (crc >> 1) ^ 0xedb88320u;
			else
				crc >>= 1;
		}
	}
	return crc;
}

/* ------------------------------------------------------- (de)serialising --- */
static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t mcufw_config_encode(const mcufw_state_t *st, uint8_t *out, size_t cap)
{
	size_t i;
	uint32_t crc;

	if (!st || !out || cap < MCUFW_CONFIG_SIZE)
		return 0u;

	for (i = 0; i < MCUFW_CONFIG_SIZE; i++)
		out[i] = 0u;

	put32(out + 0, MCUFW_CONFIG_MAGIC);
	put16(out + 4, MCUFW_CONFIG_VERSION);
	out[6] = st->power ? 1u : 0u;
	out[7] = mcufw_state_clamp_bright((long)st->bright);
	put16(out + 8, mcufw_state_clamp_ct((long)st->ct));
	put16(out + 10, mcufw_state_clamp_hue((long)st->hue));
	out[12] = mcufw_state_clamp_sat((long)st->sat);
	out[13] = (st->color_mode == MCUFW_COLOR_MODE_HSV)
			  ? (uint8_t)MCUFW_COLOR_MODE_HSV
			  : (uint8_t)MCUFW_COLOR_MODE_CT;

	/* Same sanitising rule as mcufw_state_set_name(): JSON-safe, control
	 * characters and quote/backslash become '_'.  Persist limit is 30. */
	for (i = 0; i < MCUFW_CONFIG_NAME_MAX && st->name[i]; i++) {
		char c = st->name[i];

		if (c != '"' && c != '\\' && (unsigned char)c >= 0x20u)
			out[14u + i] = (uint8_t)c;
		else
			out[14u + i] = (uint8_t)'_';
	}

	crc = mcufw_config_crc32(out, MCUFW_CONFIG_CRC_OFF);
	put32(out + MCUFW_CONFIG_CRC_OFF, crc);
	return MCUFW_CONFIG_SIZE;
}

/* Validate a raw block WITHOUT touching the state.  Returns MCUFW_CFG_OK or
 * the precise rejection reason. */
static int validate(const uint8_t *b)
{
	uint32_t magic = get32(b + 0);
	uint16_t ver = get16(b + 4);
	uint32_t want = get32(b + MCUFW_CONFIG_CRC_OFF);
	uint32_t have = mcufw_config_crc32(b, MCUFW_CONFIG_CRC_OFF);

	if (magic != MCUFW_CONFIG_MAGIC)
		return MCUFW_CFG_ERR_MAGIC;
	if (ver != MCUFW_CONFIG_VERSION)
		return MCUFW_CFG_ERR_VERSION;
	if (want != have)
		return MCUFW_CFG_ERR_CRC;
	return MCUFW_CFG_OK;
}

/* Copy the payload into the state.  Only ever called for a VALIDATED block,
 * which is what keeps foreign_fields at 0 for corrupt images. */
static void decode(const uint8_t *b, mcufw_state_t *st)
{
	size_t i;

	st->power = b[6] ? 1u : 0u;
	st->bright = mcufw_state_clamp_bright((long)b[7]);
	st->ct = mcufw_state_clamp_ct((long)get16(b + 8));
	st->hue = mcufw_state_clamp_hue((long)get16(b + 10));
	st->sat = mcufw_state_clamp_sat((long)b[12]);
	st->color_mode = (b[13] == MCUFW_COLOR_MODE_HSV)
				 ? (uint8_t)MCUFW_COLOR_MODE_HSV
				 : (uint8_t)MCUFW_COLOR_MODE_CT;
	for (i = 0; i < MCUFW_CONFIG_NAME_MAX; i++)
		st->name[i] = (char)b[14u + i];
	st->name[MCUFW_CONFIG_NAME_MAX] = '\0';
}

/* ------------------------------------------------------------------- api --- */
void mcufw_config_init(void)
{
	size_t i;

	g_cfg.loads = 0u;
	g_cfg.load_ok = 0u;
	g_cfg.load_defaults = 0u;
	g_cfg.saves = 0u;
	g_cfg.save_ok = 0u;
	g_cfg.save_failed = 0u;
	g_cfg.save_skipped = 0u;
	g_cfg.hal_writes = 0u;
	g_cfg.foreign_fields = 0u;
	g_cfg.logs = 0u;
	g_cfg.last_status = MCUFW_CFG_OK;
	g_cfg.last_log[0] = '\0';
	for (i = 0; i < MCUFW_CONFIG_SIZE; i++)
		g_last[i] = 0u;
	g_have_last = 0;
	g_saved_mutations = 0u;
	g_armed = 0;
}

int mcufw_config_load(mcufw_state_t *st)
{
	uint8_t b[MCUFW_CONFIG_SIZE];
	int rc;

	if (!st)
		return MCUFW_CFG_ERR_ARG;

	g_cfg.loads++;

	if (mcufw_config_hal_read(b, MCUFW_CONFIG_SIZE) != 0) {
		g_cfg.load_defaults++;
		g_cfg.last_status = MCUFW_CFG_ERR_READ;
		cfg_log("unreadable", 0u);
		return MCUFW_CFG_ERR_READ;
	}

	rc = validate(b);
	if (rc != MCUFW_CFG_OK) {
		/* Defaults stay in place: NOTHING is copied out of the bad
		 * block, so foreign_fields remains 0 by construction. */
		g_cfg.load_defaults++;
		g_cfg.last_status = rc;
		switch (rc) {
		case MCUFW_CFG_ERR_MAGIC:
			cfg_log("bad-magic", get32(b + 0));
			break;
		case MCUFW_CFG_ERR_VERSION:
			cfg_log("bad-version", (uint32_t)get16(b + 4));
			break;
		default:
			cfg_log("bad-crc", get32(b + MCUFW_CONFIG_CRC_OFF));
			break;
		}
		return rc;
	}

	decode(b, st);
	g_cfg.load_ok++;
	g_cfg.last_status = MCUFW_CFG_OK;
	return MCUFW_CFG_OK;
}

int mcufw_config_save(const mcufw_state_t *st)
{
	uint8_t b[MCUFW_CONFIG_SIZE];
	size_t i;
	int same;

	if (!st)
		return MCUFW_CFG_ERR_ARG;

	g_cfg.saves++;
	if (mcufw_config_encode(st, b, sizeof(b)) != MCUFW_CONFIG_SIZE)
		return MCUFW_CFG_ERR_ARG;

	same = g_have_last;
	if (same) {
		for (i = 0; i < MCUFW_CONFIG_SIZE; i++) {
			if (g_last[i] != b[i]) {
				same = 0;
				break;
			}
		}
	}
	if (same) {
		g_cfg.save_skipped++;
		return MCUFW_CFG_OK;
	}

	g_cfg.hal_writes++;
	if (mcufw_config_hal_write(b, MCUFW_CONFIG_SIZE) != 0) {
		g_cfg.save_failed++;
		return MCUFW_CFG_ERR_WRITE;
	}
	for (i = 0; i < MCUFW_CONFIG_SIZE; i++)
		g_last[i] = b[i];
	g_have_last = 1;
	g_cfg.save_ok++;
	return MCUFW_CFG_OK;
}

int mcufw_config_boot(mcufw_state_t *st)
{
	int rc;

	if (!st)
		return MCUFW_CFG_ERR_ARG;

	mcufw_state_init(st);
	rc = mcufw_config_load(st);
	/* Loading is not a user action: the mutation counter starts at 0 so the
	 * first idle ticks cannot trigger a save. */
	st->mutations = 0u;
	g_saved_mutations = 0u;
	g_armed = 1;
	return rc;
}

int mcufw_config_service(mcufw_state_t *st)
{
	if (!st || !g_armed)
		return 0;
	if (st->mutations == g_saved_mutations)
		return 0;

	g_saved_mutations = st->mutations;
	if (mcufw_config_save(st) == MCUFW_CFG_OK)
		return 1;
	return 0;
}

const mcufw_config_stats_t *mcufw_config_stats(void)
{
	return &g_cfg;
}
