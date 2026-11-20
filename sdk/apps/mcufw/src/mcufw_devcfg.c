/*
 * mcufw_devcfg.c -- CRC-32 persistent extended device config + JSON API.
 */
#include "mcufw_devcfg.h"
#include "mcufw_config.h"
#include "mcufw_config_hal.h"
#include "mcufw_wlan.h"

static mcufw_devcfg_t g_devcfg;
static int g_devcfg_init;

static size_t slen_n(const char *s, size_t max)
{
	size_t n = 0u;
	if (!s) return 0u;
	while (n < max && s[n]) n++;
	return n;
}

static void scpy_n(char *dst, const char *src, size_t cap)
{
	size_t i = 0u;
	if (!dst || cap == 0u) return;
	if (src) {
		while (src[i] && i + 1u < cap) {
			dst[i] = src[i];
			i++;
		}
	}
	while (i < cap)
		dst[i++] = '\0';
}

static int seq_s(const char *a, const char *b)
{
	while (*a && *b && *a == *b) { a++; b++; }
	return *a == '\0' && *b == '\0';
}

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int devcfg_validate(const mcufw_devcfg_t *c)
{
	size_t sl, pl, hl;

	if (!c) return -1;
	sl = slen_n(c->wifi_ssid, sizeof(c->wifi_ssid));
	pl = slen_n(c->wifi_psk, sizeof(c->wifi_psk));
	hl = slen_n(c->mqtt_host, sizeof(c->mqtt_host));
	if (c->wifi_provisioned) {
		if (sl < 1u || sl > 32u) return -1;
		if (pl != 0u && (pl < 8u || pl > 63u)) return -1;
	} else {
		if (sl > 32u) return -1;
		if (pl != 0u && (pl < 8u || pl > 63u)) return -1;
	}
	if (hl > 39u) return -1;
	if (c->mqtt_enabled && hl == 0u) return -1;
	if (c->mqtt_port == 0u) return -1;
	if (c->power_on_mode > MCUFW_POWERON_DEFAULT) return -1;
	if (c->pwm_freq_hz < MCUFW_PWM_FREQ_MIN || c->pwm_freq_hz > MCUFW_PWM_FREQ_MAX)
		return -1;
	if (c->default_transition_ms > MCUFW_TRANS_MAX_MS) return -1;
	return 0;
}

void mcufw_devcfg_defaults(mcufw_devcfg_t *cfg)
{
	if (!cfg) return;
	for (size_t i = 0; i < sizeof(*cfg); i++)
		((uint8_t *)cfg)[i] = 0u;
	scpy_n(cfg->mqtt_host, "192.168.1.106", sizeof(cfg->mqtt_host));
	cfg->mqtt_port = 1883u;
	cfg->mqtt_enabled = 1u;
	cfg->power_on_mode = MCUFW_POWERON_LAST;
	cfg->pwm_freq_hz = MCUFW_PWM_FREQ_DEFAULT;
	cfg->default_transition_ms = 400u;
	cfg->gamma_enable = 1u;
}

mcufw_devcfg_t *mcufw_devcfg(void)
{
	if (!g_devcfg_init) {
		mcufw_devcfg_defaults(&g_devcfg);
		g_devcfg_init = 1;
	}
	return &g_devcfg;
}

const char *mcufw_devcfg_poweron_name(uint8_t mode)
{
	switch (mode) {
	case MCUFW_POWERON_LAST:    return "last";
	case MCUFW_POWERON_OFF:     return "off";
	case MCUFW_POWERON_DEFAULT: return "on_default";
	default:                    return "last";
	}
}

size_t mcufw_devcfg_encode(const mcufw_devcfg_t *cfg, uint8_t *out, size_t cap)
{
	size_t off = 0u;

	if (!cfg || !out || cap < MCUFW_DEVCFG_WIRE_SIZE)
		return 0u;
	for (size_t i = 0; i < MCUFW_DEVCFG_WIRE_SIZE; i++)
		out[i] = 0u;

	wr32(out + 0, MCUFW_DEVCFG_MAGIC);
	wr16(out + 4, MCUFW_DEVCFG_VERSION);
	out[6] = cfg->wifi_provisioned ? 1u : 0u;
	out[7] = cfg->mqtt_enabled ? 1u : 0u;
	off = 8u;
	for (size_t i = 0; i < 33u; i++) out[off++] = (uint8_t)cfg->wifi_ssid[i];
	for (size_t i = 0; i < 64u; i++) out[off++] = (uint8_t)cfg->wifi_psk[i];
	for (size_t i = 0; i < 40u; i++) out[off++] = (uint8_t)cfg->mqtt_host[i];
	wr16(out + off, cfg->mqtt_port); off += 2u;
	for (size_t i = 0; i < 32u; i++) out[off++] = (uint8_t)cfg->mqtt_user[i];
	for (size_t i = 0; i < 32u; i++) out[off++] = (uint8_t)cfg->mqtt_pass[i];
	out[off++] = cfg->power_on_mode;
	wr16(out + off, cfg->pwm_freq_hz); off += 2u;
	wr16(out + off, cfg->default_transition_ms); off += 2u;
	out[off++] = cfg->gamma_enable ? 1u : 0u;
	/* off == 217; bytes 217..219 reserved (0); bytes 220..223 = crc32 */
	wr32(out + MCUFW_DEVCFG_WIRE_SIZE - 4u,
	     mcufw_config_crc32(out, MCUFW_DEVCFG_WIRE_SIZE - 4u));
	return MCUFW_DEVCFG_WIRE_SIZE;
}

int mcufw_devcfg_decode(const uint8_t *in, size_t len, mcufw_devcfg_t *out)
{
	mcufw_devcfg_t tmp;
	size_t off = 8u;
	uint32_t expected_crc, got_crc;

	if (!in || !out || len != MCUFW_DEVCFG_WIRE_SIZE)
		return -1;
	if (rd32(in + 0) != MCUFW_DEVCFG_MAGIC)
		return -1;
	if (rd16(in + 4) != MCUFW_DEVCFG_VERSION)
		return -1;
	expected_crc = mcufw_config_crc32(in, MCUFW_DEVCFG_WIRE_SIZE - 4u);
	got_crc = rd32(in + MCUFW_DEVCFG_WIRE_SIZE - 4u);
	if (expected_crc != got_crc)
		return -1;

	mcufw_devcfg_defaults(&tmp);
	tmp.wifi_provisioned = in[6] ? 1u : 0u;
	tmp.mqtt_enabled = in[7] ? 1u : 0u;
	for (size_t i = 0; i < 33u; i++) tmp.wifi_ssid[i] = (char)in[off++];
	tmp.wifi_ssid[32] = '\0';
	for (size_t i = 0; i < 64u; i++) tmp.wifi_psk[i] = (char)in[off++];
	tmp.wifi_psk[63] = '\0';
	for (size_t i = 0; i < 40u; i++) tmp.mqtt_host[i] = (char)in[off++];
	tmp.mqtt_host[39] = '\0';
	tmp.mqtt_port = rd16(in + off); off += 2u;
	for (size_t i = 0; i < 32u; i++) tmp.mqtt_user[i] = (char)in[off++];
	tmp.mqtt_user[31] = '\0';
	for (size_t i = 0; i < 32u; i++) tmp.mqtt_pass[i] = (char)in[off++];
	tmp.mqtt_pass[31] = '\0';
	tmp.power_on_mode = in[off++];
	tmp.pwm_freq_hz = rd16(in + off); off += 2u;
	tmp.default_transition_ms = rd16(in + off); off += 2u;
	tmp.gamma_enable = in[off++] ? 1u : 0u;

	if (devcfg_validate(&tmp) != 0)
		return -1;
	*out = tmp;
	return 0;
}

static void sync_wlan(const mcufw_devcfg_t *c)
{
	(void)mcufw_wlan_set_cfg(c->wifi_ssid, c->wifi_psk, c->wifi_provisioned);
}

int mcufw_devcfg_boot(void)
{
	uint8_t raw[MCUFW_DEVCFG_WIRE_SIZE];
	mcufw_devcfg_t *c = mcufw_devcfg();

	if (mcufw_devcfg_hal_read(raw, sizeof(raw)) == 0 &&
	    mcufw_devcfg_decode(raw, sizeof(raw), c) == 0) {
		sync_wlan(c);
		return 0;
	}
	mcufw_devcfg_defaults(c);
	sync_wlan(c);
	return -1;
}

int mcufw_devcfg_save(void)
{
	uint8_t raw[MCUFW_DEVCFG_WIRE_SIZE];
	const mcufw_devcfg_t *c = mcufw_devcfg();

	if (devcfg_validate(c) != 0)
		return -1;
	if (mcufw_devcfg_encode(c, raw, sizeof(raw)) != sizeof(raw))
		return -1;
	if (mcufw_devcfg_hal_write(raw, sizeof(raw)) != 0)
		return -1;
	sync_wlan(c);
	return 0;
}

int mcufw_devcfg_apply_power_on(mcufw_state_t *st)
{
	const mcufw_devcfg_t *c = mcufw_devcfg();

	if (!st) return -1;
	if (c->power_on_mode == MCUFW_POWERON_OFF) {
		st->power = 0u;
	} else if (c->power_on_mode == MCUFW_POWERON_DEFAULT) {
		st->power = 1u;
		st->bright = 100u;
		st->ct = 4000u;
		st->color_mode = MCUFW_COLOR_MODE_CT;
	}
	return 0;
}

/* --- JSON encode/parse helpers (freestanding) ---------------------------- */
static void j_str(char *o, size_t cap, size_t *p, const char *s)
{
	while (s && *s && *p + 1u < cap)
		o[(*p)++] = *s++;
	o[*p] = '\0';
}

static void j_esc(char *o, size_t cap, size_t *p, const char *s)
{
	j_str(o, cap, p, "\"");
	while (s && *s && *p + 2u < cap) {
		char ch = *s++;
		if (ch == '"' || ch == '\\') {
			o[(*p)++] = '\\';
			o[(*p)++] = ch;
		} else if ((uint8_t)ch >= 0x20u) {
			o[(*p)++] = ch;
		}
	}
	j_str(o, cap, p, "\"");
}

static void j_u32(char *o, size_t cap, size_t *p, uint32_t v)
{
	char b[12];
	int i = 0;
	if (v == 0u) b[i++] = '0';
	else while (v && i < 11) { b[i++] = (char)('0' + (v % 10u)); v /= 10u; }
	while (i-- > 0 && *p + 1u < cap) o[(*p)++] = b[i];
	o[*p] = '\0';
}

int mcufw_devcfg_to_json(const mcufw_devcfg_t *cfg, char *out, size_t cap)
{
	size_t p = 0u;

	if (!cfg || !out || cap < 64u) return -1;
	j_str(out, cap, &p, "{\"wifi_ssid\":");
	j_esc(out, cap, &p, cfg->wifi_ssid);
	j_str(out, cap, &p, ",\"wifi_psk\":");
	j_esc(out, cap, &p, cfg->wifi_psk[0] ? "***" : "");
	j_str(out, cap, &p, ",\"wifi_provisioned\":");
	j_u32(out, cap, &p, cfg->wifi_provisioned);
	j_str(out, cap, &p, ",\"mqtt_host\":");
	j_esc(out, cap, &p, cfg->mqtt_host);
	j_str(out, cap, &p, ",\"mqtt_port\":");
	j_u32(out, cap, &p, cfg->mqtt_port);
	j_str(out, cap, &p, ",\"mqtt_user\":");
	j_esc(out, cap, &p, cfg->mqtt_user);
	j_str(out, cap, &p, ",\"mqtt_pass\":");
	j_esc(out, cap, &p, cfg->mqtt_pass[0] ? "***" : "");
	j_str(out, cap, &p, ",\"mqtt_enabled\":");
	j_u32(out, cap, &p, cfg->mqtt_enabled);
	j_str(out, cap, &p, ",\"power_on_mode\":");
	j_esc(out, cap, &p, mcufw_devcfg_poweron_name(cfg->power_on_mode));
	j_str(out, cap, &p, ",\"pwm_freq_hz\":");
	j_u32(out, cap, &p, cfg->pwm_freq_hz);
	j_str(out, cap, &p, ",\"default_transition_ms\":");
	j_u32(out, cap, &p, cfg->default_transition_ms);
	j_str(out, cap, &p, ",\"gamma_enable\":");
	j_u32(out, cap, &p, cfg->gamma_enable);
	j_str(out, cap, &p, "}");
	return (p + 1u < cap) ? (int)p : -1;
}

static int find_json_val(const char *j, size_t len, const char *key, const char **vstart)
{
	size_t klen = slen_n(key, 64u);

	for (size_t i = 0; i + klen + 2u < len; i++) {
		size_t k;
		if (j[i] != '"') continue;
		for (k = 0; k < klen && i + 1u + k < len; k++) {
			if (j[i + 1u + k] != key[k]) break;
		}
		if (k != klen || j[i + 1u + k] != '"') continue;
		size_t p = i + 2u + klen;
		while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n'))
			p++;
		if (p >= len || j[p] != ':') continue;
		p++;
		while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n'))
			p++;
		if (p < len) {
			*vstart = j + p;
			return 1;
		}
	}
	return 0;
}

/* Returns 1 if key present & copied, 0 if absent, -1 if malformed/too long. */
static int jget_str(const char *j, size_t len, const char *key, char *out, size_t cap)
{
	const char *v = NULL;
	size_t o = 0u, rem;

	if (!find_json_val(j, len, key, &v)) return 0;
	if (*v != '"') return -1;
	v++;
	rem = len - (size_t)(v - j);
	for (size_t i = 0; i < rem; i++) {
		char ch = v[i];
		if (ch == '"') {
			out[o] = '\0';
			return 1;
		}
		if (ch == '\\' && i + 1u < rem)
			ch = v[++i];
		if (o + 1u >= cap)
			return -1;
		out[o++] = ch;
	}
	return -1;
}

/* Returns 1 if present & integer parsed, 0 if absent, -1 if malformed. */
static int jget_long(const char *j, size_t len, const char *key, long *out)
{
	const char *v = NULL;
	long val = 0;
	int neg = 0, digits = 0;
	size_t rem, i = 0u;

	if (!find_json_val(j, len, key, &v)) return 0;
	rem = len - (size_t)(v - j);
	if (i < rem && v[i] == '-') { neg = 1; i++; }
	while (i < rem && v[i] >= '0' && v[i] <= '9') {
		val = val * 10L + (long)(v[i] - '0');
		digits++;
		i++;
		if (digits > 9) return -1;
	}
	if (digits == 0) return -1;
	*out = neg ? -val : val;
	return 1;
}

int mcufw_devcfg_apply_json(const char *json, size_t len)
{
	mcufw_devcfg_t next;
	char sbuf[72];
	long lval = 0;
	int rc;

	if (!json || len < 2u) return -1;
	next = *mcufw_devcfg();

	rc = jget_str(json, len, "wifi_ssid", sbuf, sizeof(sbuf));
	if (rc < 0 || (rc > 0 && slen_n(sbuf, sizeof(sbuf)) > 32u)) return -1;
	if (rc > 0) {
		scpy_n(next.wifi_ssid, sbuf, sizeof(next.wifi_ssid));
		if (next.wifi_ssid[0] != '\0')
			next.wifi_provisioned = 1u;
	}

	rc = jget_str(json, len, "wifi_psk", sbuf, sizeof(sbuf));
	if (rc < 0 || (rc > 0 && slen_n(sbuf, sizeof(sbuf)) > 63u)) return -1;
	if (rc > 0 && !seq_s(sbuf, "***"))
		scpy_n(next.wifi_psk, sbuf, sizeof(next.wifi_psk));

	rc = jget_long(json, len, "wifi_provisioned", &lval);
	if (rc < 0 || (rc > 0 && (lval < 0 || lval > 1))) return -1;
	if (rc > 0) next.wifi_provisioned = (uint8_t)lval;

	rc = jget_str(json, len, "mqtt_host", sbuf, sizeof(sbuf));
	if (rc < 0 || (rc > 0 && slen_n(sbuf, sizeof(sbuf)) > 39u)) return -1;
	if (rc > 0) scpy_n(next.mqtt_host, sbuf, sizeof(next.mqtt_host));

	rc = jget_long(json, len, "mqtt_port", &lval);
	if (rc < 0 || (rc > 0 && (lval < 1 || lval > 65535))) return -1;
	if (rc > 0) next.mqtt_port = (uint16_t)lval;

	rc = jget_str(json, len, "mqtt_user", sbuf, sizeof(sbuf));
	if (rc < 0 || (rc > 0 && slen_n(sbuf, sizeof(sbuf)) > 31u)) return -1;
	if (rc > 0) scpy_n(next.mqtt_user, sbuf, sizeof(next.mqtt_user));

	rc = jget_str(json, len, "mqtt_pass", sbuf, sizeof(sbuf));
	if (rc < 0 || (rc > 0 && slen_n(sbuf, sizeof(sbuf)) > 31u)) return -1;
	if (rc > 0 && !seq_s(sbuf, "***"))
		scpy_n(next.mqtt_pass, sbuf, sizeof(next.mqtt_pass));

	rc = jget_long(json, len, "mqtt_enabled", &lval);
	if (rc < 0 || (rc > 0 && (lval < 0 || lval > 1))) return -1;
	if (rc > 0) next.mqtt_enabled = (uint8_t)lval;

	rc = jget_str(json, len, "power_on_mode", sbuf, sizeof(sbuf));
	if (rc > 0) {
		if (seq_s(sbuf, "last")) next.power_on_mode = MCUFW_POWERON_LAST;
		else if (seq_s(sbuf, "off")) next.power_on_mode = MCUFW_POWERON_OFF;
		else if (seq_s(sbuf, "on_default") || seq_s(sbuf, "default"))
			next.power_on_mode = MCUFW_POWERON_DEFAULT;
		else return -1;
	} else {
		rc = jget_long(json, len, "power_on_mode", &lval);
		if (rc < 0 || (rc > 0 && (lval < 0 || lval > 2))) return -1;
		if (rc > 0) next.power_on_mode = (uint8_t)lval;
	}

	rc = jget_long(json, len, "pwm_freq_hz", &lval);
	if (rc < 0 || (rc > 0 && (lval < (long)MCUFW_PWM_FREQ_MIN || lval > (long)MCUFW_PWM_FREQ_MAX)))
		return -1;
	if (rc > 0) next.pwm_freq_hz = (uint16_t)lval;

	rc = jget_long(json, len, "default_transition_ms", &lval);
	if (rc < 0 || (rc > 0 && (lval < 0 || lval > (long)MCUFW_TRANS_MAX_MS)))
		return -1;
	if (rc > 0) next.default_transition_ms = (uint16_t)lval;

	rc = jget_long(json, len, "gamma_enable", &lval);
	if (rc < 0 || (rc > 0 && (lval < 0 || lval > 1))) return -1;
	if (rc > 0) next.gamma_enable = (uint8_t)lval;

	if (devcfg_validate(&next) != 0)
		return -1;
	*mcufw_devcfg() = next;
	g_devcfg_init = 1;
	return mcufw_devcfg_save();
}

int mcufw_devcfg_set_power_on_mode(uint8_t mode)
{
	if (mode > MCUFW_POWERON_DEFAULT) return -1;
	mcufw_devcfg()->power_on_mode = mode;
	return mcufw_devcfg_save();
}

int mcufw_devcfg_set_pwm_freq(uint16_t hz)
{
	if (hz < MCUFW_PWM_FREQ_MIN || hz > MCUFW_PWM_FREQ_MAX) return -1;
	mcufw_devcfg()->pwm_freq_hz = hz;
	return mcufw_devcfg_save();
}

int mcufw_devcfg_set_transition_ms(uint16_t ms)
{
	if (ms > MCUFW_TRANS_MAX_MS) return -1;
	mcufw_devcfg()->default_transition_ms = ms;
	return mcufw_devcfg_save();
}
