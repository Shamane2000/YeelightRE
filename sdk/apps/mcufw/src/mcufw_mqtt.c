/*
 * mcufw_mqtt.c -- Clean-room MQTT 3.1.1 client + HA 2026.11 Auto-Discovery.
 *
 * Phase 3.5 payload builders remain intact for host_test compatibility.
 * Phase 4b.4 adds a full non-blocking MQTT 3.1.1 wire codec & state machine
 * driven by mcufw_runtime_tick() behind an injectable mcufw_mqtt_io_t seam.
 */
#include "mcufw_mqtt.h"
#include "mcufw_devcfg.h"
#include "mcufw_state.h"
#include "mcufw_ota.h"
#include "mcufw_rollback_hal.h"

/* ----------------------------------------------------- string helpers --- */
static void append(char *out, size_t cap, size_t *pos, const char *s)
{
	if (!out || cap == 0u || !pos || !s)
		return;
	while (*s && *pos + 1u < cap)
		out[(*pos)++] = *s++;
	out[*pos] = '\0';
}

static void append_dec(char *out, size_t cap, size_t *pos, uint32_t v)
{
	char buf[12];
	int i = 0;

	if (!out || cap == 0u || !pos)
		return;
	if (v == 0u) {
		buf[i++] = '0';
	} else {
		while (v && i < 11) {
			buf[i++] = (char)('0' + (int)(v % 10u));
			v /= 10u;
		}
	}
	while (i > 0 && *pos + 1u < cap)
		out[(*pos)++] = buf[--i];
	out[*pos] = '\0';
}

static void append_sdec(char *out, size_t cap, size_t *pos, int v)
{
	if (v < 0) {
		append(out, cap, pos, "-");
		append_dec(out, cap, pos, (uint32_t)(-(long)v));
	} else {
		append_dec(out, cap, pos, (uint32_t)v);
	}
}

static void append_hex(char *out, size_t cap, size_t *pos, uint32_t v, int digits)
{
	static const char hx[] = "0123456789abcdef";

	if (!out || cap == 0u || !pos)
		return;
	for (int d = digits - 1; d >= 0 && *pos + 1u < cap; d--)
		out[(*pos)++] = hx[(v >> (unsigned)(d * 4)) & 0xfu];
	out[*pos] = '\0';
}

static size_t slen(const char *s)
{
	size_t n = 0u;
	while (s && s[n]) n++;
	return n;
}

static void scpy(char *dst, size_t cap, const char *src)
{
	size_t i = 0u;
	if (!dst || cap == 0u)
		return;
	if (src) {
		while (src[i] && i + 1u < cap) {
			dst[i] = src[i];
			i++;
		}
	}
	dst[i] = '\0';
}

static int seq(const char *a, const char *b)
{
	size_t i = 0u;
	if (!a || !b)
		return 0;
	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

static int s_ends_with(const char *s, const char *suffix)
{
	size_t ls = slen(s);
	size_t lf = slen(suffix);
	if (ls < lf)
		return 0;
	return seq(s + (ls - lf), suffix);
}

/* ------------------------------------------- Phase 3.5 public helpers --- */
int mcufw_mqtt_discovery_topic(uint32_t muid, char *out, size_t cap)
{
	size_t pos = 0u;
	if (!out || cap == 0u)
		return 0;
	append(out, cap, &pos, "homeassistant/light/");
	append_hex(out, cap, &pos, muid, 8);
	append(out, cap, &pos, "/config");
	return (int)pos;
}

int mcufw_mqtt_state_topic(uint32_t muid, char *out, size_t cap)
{
	size_t pos = 0u;
	if (!out || cap == 0u)
		return 0;
	append(out, cap, &pos, "yeelight/");
	append_hex(out, cap, &pos, muid, 8);
	append(out, cap, &pos, "/state");
	return (int)pos;
}

int mcufw_mqtt_cmd_topic(uint32_t muid, char *out, size_t cap)
{
	size_t pos = 0u;
	if (!out || cap == 0u)
		return 0;
	append(out, cap, &pos, "yeelight/");
	append_hex(out, cap, &pos, muid, 8);
	append(out, cap, &pos, "/set");
	return (int)pos;
}

int mcufw_mqtt_discovery_light(uint32_t muid, char *out, size_t cap)
{
	size_t pos = 0u;
	char muid_hex[9];
	size_t hp = 0u;

	if (!out || cap == 0u)
		return 0;
	append_hex(muid_hex, sizeof(muid_hex), &hp, muid, 8);

	append(out, cap, &pos, "{\"name\":\"Yeelight ");
	append(out, cap, &pos, muid_hex);
	append(out, cap, &pos, "\",\"unique_id\":\"yeelight_");
	append(out, cap, &pos, muid_hex);
	append(out, cap, &pos, "\",\"schema\":\"json\",\"brightness\":true,"
	       "\"color_temp\":true,\"rgb\":true,"
	       "\"command_topic\":\"yeelight/");
	append(out, cap, &pos, muid_hex);
	append(out, cap, &pos, "/set\",\"state_topic\":\"yeelight/");
	append(out, cap, &pos, muid_hex);
	append(out, cap, &pos, "/state\"}");
	return (int)pos;
}

int mcufw_mqtt_state_payload(int power, uint8_t bright, uint16_t ct,
                             uint16_t hue, uint8_t sat,
                             char *out, size_t cap)
{
	size_t pos = 0u;
	if (!out || cap == 0u)
		return 0;
	append(out, cap, &pos, "{\"power\":\"");
	append(out, cap, &pos, power ? "on" : "off");
	append(out, cap, &pos, "\",\"bright\":");
	append_dec(out, cap, &pos, bright);
	append(out, cap, &pos, ",\"ct\":");
	append_dec(out, cap, &pos, ct);
	append(out, cap, &pos, ",\"hue\":");
	append_dec(out, cap, &pos, hue);
	append(out, cap, &pos, ",\"sat\":");
	append_dec(out, cap, &pos, sat);
	append(out, cap, &pos, "}");
	return (int)pos;
}

/* ------------------------------------------- depth-1 JSON mini parser --- */
static size_t skip_ws(const char *s, size_t len, size_t p)
{
	while (p < len && (s[p] == ' ' || s[p] == '\t' ||
	                   s[p] == '\r' || s[p] == '\n'))
		p++;
	return p;
}

static int json_find_val_d1(const char *s, size_t len, const char *key, size_t *val_off)
{
	size_t klen = slen(key);
	int depth = 0;
	int in_str = 0;

	for (size_t i = 0u; i < len; i++) {
		char c = s[i];
		if (in_str) {
			if (c == '\\' && i + 1u < len) {
				i++;
				continue;
			}
			if (c == '"')
				in_str = 0;
			continue;
		}
		if (c == '{' || c == '[') {
			depth++;
			continue;
		}
		if (c == '}' || c == ']') {
			depth--;
			continue;
		}
		if (c == '"') {
			if (depth == 1 && i + 1u + klen < len && s[i + 1u + klen] == '"') {
				size_t k;
				for (k = 0u; k < klen && s[i + 1u + k] == key[k]; k++) ;
				if (k == klen) {
					size_t p = skip_ws(s, len, i + 2u + klen);
					if (p < len && s[p] == ':') {
						*val_off = skip_ws(s, len, p + 1u);
						return (*val_off < len) ? 0 : -1;
					}
				}
			}
			in_str = 1;
		}
	}
	return -1;
}

static int json_d1_str(const char *s, size_t len, const char *key, char *out, size_t cap)
{
	size_t p = 0u;
	size_t o = 0u;

	if (json_find_val_d1(s, len, key, &p) != 0 || s[p] != '"')
		return -1;
	p++;
	while (p < len && s[p] != '"') {
		char c = s[p++];
		if (c == '\\' && p < len)
			c = s[p++];
		if (o + 1u < cap)
			out[o++] = c;
	}
	if (p >= len || s[p] != '"')
		return -1;
	out[o] = '\0';
	return 0;
}

static int json_d1_long(const char *s, size_t len, const char *key, long *out)
{
	size_t p = 0u;
	long sign = 1;
	long val = 0;
	int digits = 0;

	if (json_find_val_d1(s, len, key, &p) != 0)
		return -1;
	if (s[p] == '-') {
		sign = -1;
		p++;
	}
	while (p < len && s[p] >= '0' && s[p] <= '9') {
		val = val * 10L + (long)(s[p] - '0');
		p++;
		digits++;
	}
	if (digits == 0)
		return -1;
	*out = val * sign;
	return 0;
}

static int json_d1_obj(const char *s, size_t len, const char *key,
                       const char **sub, size_t *sub_len)
{
	size_t p = 0u;
	int depth = 0;
	int in_str = 0;

	if (json_find_val_d1(s, len, key, &p) != 0 || s[p] != '{')
		return -1;
	for (size_t i = p; i < len; i++) {
		char c = s[i];
		if (in_str) {
			if (c == '\\' && i + 1u < len)
				i++;
			else if (c == '"')
				in_str = 0;
			continue;
		}
		if (c == '"') {
			in_str = 1;
		} else if (c == '{') {
			depth++;
		} else if (c == '}') {
			depth--;
			if (depth == 0) {
				*sub = s + p;
				*sub_len = i - p + 1u;
				return 0;
			}
		}
	}
	return -1;
}

static int parse_trimmed_long(const char *s, size_t len, long *out)
{
	size_t p = skip_ws(s, len, 0u);
	long sign = 1;
	long v = 0;
	int digits = 0;

	if (p < len && s[p] == '-') {
		sign = -1;
		p++;
	}
	while (p < len && s[p] >= '0' && s[p] <= '9') {
		v = v * 10L + (long)(s[p] - '0');
		p++;
		digits++;
	}
	if (digits == 0)
		return -1;
	p = skip_ws(s, len, p);
	if (p != len)
		return -1;
	*out = v * sign;
	return 0;
}

static void trim_copy(const char *in, size_t len, char *out, size_t cap)
{
	size_t a = skip_ws(in, len, 0u);
	size_t b = len;
	size_t o = 0u;

	while (b > a && (in[b - 1u] == ' ' || in[b - 1u] == '\t' ||
	                 in[b - 1u] == '\r' || in[b - 1u] == '\n'))
		b--;
	while (a < b && o + 1u < cap)
		out[o++] = in[a++];
	out[o] = '\0';
}

/* ------------------------------------------- Client static storage (.bss) --- */
#define MQTT_TX_CAP       1024u
#define MQTT_RX_CAP       1024u
#define MQTT_JSON_CAP     896u
#define MQTT_TOPIC_CAP    96u
#define MQTT_BURST_ITEMS  13u

static const mcufw_mqtt_io_t *g_io;
static uint32_t g_muid = 0x05eb8034u;
static char     g_muid_hex[9] = "05eb8034";

static mcufw_mqtt_client_stats_t g_stats;
static uint32_t g_phase_ms;
static uint32_t g_backoff_start_ms;
static uint32_t g_last_tx_ms;
static uint32_t g_ping_sent_ms;
static uint32_t g_last_diag_ms;
static uint32_t g_start_ms;
static uint32_t g_last_pub_mutations;
static mcufw_ota_status_t g_last_ota_status;
static uint8_t  g_burst_step;
static uint8_t  g_ping_outstanding;
static uint8_t  g_config_dirty;
static uint8_t  g_restart_ticks;

static char     g_active_host[40];
static uint16_t g_active_port;
static char     g_active_user[32];

static uint8_t  g_tx_pkt[MQTT_TX_CAP];
static size_t   g_tx_len;
static size_t   g_tx_sent;

static uint8_t  g_rx_acc[MQTT_RX_CAP];
static size_t   g_rx_len;

static char     g_json_buf[MQTT_JSON_CAP];
static char     g_topic_buf[MQTT_TOPIC_CAP];

static uint32_t cur_ms(void)
{
	return (g_io && g_io->now_ms) ? g_io->now_ms() : 0u;
}

static void refresh_muid_hex(void)
{
	size_t hp = 0u;
	append_hex(g_muid_hex, sizeof(g_muid_hex), &hp, g_muid, 8);
}

static void close_conn(mcufw_mqtt_conn_state_t next_state)
{
	if (g_io && g_io->tcp_close)
		g_io->tcp_close();
	g_tx_len = 0u;
	g_tx_sent = 0u;
	g_rx_len = 0u;
	g_ping_outstanding = 0u;
	g_stats.state = next_state;
}

static void enter_backoff(uint32_t now)
{
	close_conn(MCUFW_MQTT_ST_BACKOFF);
	g_stats.disconnects++;
	if (g_stats.backoff_ms == 0u) {
		g_stats.backoff_ms = MCUFW_MQTT_BACKOFF_MIN_MS;
	} else {
		g_stats.backoff_ms *= 2u;
		if (g_stats.backoff_ms > MCUFW_MQTT_BACKOFF_MAX_MS)
			g_stats.backoff_ms = MCUFW_MQTT_BACKOFF_MAX_MS;
	}
	g_backoff_start_ms = now;
}

/* ------------------------------------------- MQTT 3.1.1 wire encoding --- */
static void wr_u16(uint8_t *dst, size_t *pos, uint16_t v)
{
	dst[(*pos)++] = (uint8_t)(v >> 8);
	dst[(*pos)++] = (uint8_t)(v & 0xffu);
}

static int wr_utf8(uint8_t *dst, size_t cap, size_t *pos, const char *s)
{
	size_t n = slen(s);
	if (*pos + 2u + n > cap)
		return -1;
	wr_u16(dst, pos, (uint16_t)n);
	for (size_t i = 0u; i < n; i++)
		dst[(*pos)++] = (uint8_t)s[i];
	return 0;
}

static size_t encode_rem_len(uint8_t *out, uint32_t rem)
{
	size_t n = 0u;
	do {
		uint8_t b = (uint8_t)(rem & 0x7fu);
		rem >>= 7;
		if (rem > 0u)
			b |= 0x80u;
		out[n++] = b;
	} while (rem > 0u && n < 4u);
	return n;
}

static int flush_tx(uint32_t now)
{
	while (g_tx_sent < g_tx_len) {
		int n = g_io->tcp_send(g_tx_pkt + g_tx_sent, g_tx_len - g_tx_sent);
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		g_tx_sent += (size_t)n;
		g_last_tx_ms = now;
	}
	if (g_tx_sent >= g_tx_len) {
		g_tx_len = 0u;
		g_tx_sent = 0u;
	}
	return 0;
}

static int queue_connect(uint32_t now)
{
	const mcufw_devcfg_t *cfg = mcufw_devcfg();
	char client_id[24];
	char will_topic[48];
	uint8_t rem_hdr[4];
	uint8_t flags = 0x02u | 0x04u | 0x20u; /* CleanSession + Will + WillRetain */
	size_t p = 0u;
	size_t vh_start = 5u;
	size_t rem_len, fh_len;

	p = 0u;
	append(client_id, sizeof(client_id), &p, "yeelight_");
	append(client_id, sizeof(client_id), &p, g_muid_hex);

	p = 0u;
	append(will_topic, sizeof(will_topic), &p, "yeelight/");
	append(will_topic, sizeof(will_topic), &p, g_muid_hex);
	append(will_topic, sizeof(will_topic), &p, "/availability");

	if (cfg->mqtt_user[0]) {
		flags |= 0x80u;
		if (cfg->mqtt_pass[0])
			flags |= 0x40u;
	}

	p = vh_start;
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, "MQTT") != 0 || p + 4u > sizeof(g_tx_pkt))
		return -1;
	g_tx_pkt[p++] = 0x04u;
	g_tx_pkt[p++] = flags;
	wr_u16(g_tx_pkt, &p, MCUFW_MQTT_KEEPALIVE_S);

	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, client_id) != 0) return -1;
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, will_topic) != 0) return -1;
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, "offline") != 0) return -1;
	if ((flags & 0x80u) && wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, cfg->mqtt_user) != 0)
		return -1;
	if ((flags & 0x40u) && wr_utf8(g_tx_pkt, sizeof(g_tx_pkt), &p, cfg->mqtt_pass) != 0)
		return -1;

	rem_len = p - vh_start;
	fh_len = 1u + encode_rem_len(rem_hdr, (uint32_t)rem_len);
	g_tx_pkt[0] = 0x10u;
	for (size_t i = 0u; i < fh_len - 1u; i++)
		g_tx_pkt[1u + i] = rem_hdr[i];
	if (fh_len != vh_start) {
		for (size_t i = 0u; i < rem_len; i++)
			g_tx_pkt[fh_len + i] = g_tx_pkt[vh_start + i];
	}
	g_tx_len = fh_len + rem_len;
	g_tx_sent = 0u;
	return flush_tx(now);
}

static int queue_publish(const char *topic, const char *payload, size_t plen,
                         int retain, uint32_t now)
{
	size_t tlen = slen(topic);
	uint32_t rem_len = (uint32_t)(2u + tlen + plen);
	uint8_t rem_hdr[4];
	size_t rl_bytes = encode_rem_len(rem_hdr, rem_len);
	size_t pos = 0u;

	if (1u + rl_bytes + rem_len > sizeof(g_tx_pkt))
		return -1;
	g_tx_pkt[pos++] = (uint8_t)(0x30u | (retain ? 0x01u : 0x00u));
	for (size_t i = 0u; i < rl_bytes; i++)
		g_tx_pkt[pos++] = rem_hdr[i];
	wr_u16(g_tx_pkt, &pos, (uint16_t)tlen);
	for (size_t i = 0u; i < tlen; i++)
		g_tx_pkt[pos++] = (uint8_t)topic[i];
	for (size_t i = 0u; i < plen; i++)
		g_tx_pkt[pos++] = (uint8_t)payload[i];

	g_tx_len = pos;
	g_tx_sent = 0u;
	g_stats.pub_tx++;
	return flush_tx(now);
}

static int queue_subscribe(uint32_t now)
{
	char f0[32], f1[40], f2[40];
	size_t p = 0u;
	uint8_t rem_hdr[4];
	size_t vh_start = 5u;
	size_t rem_len, fh_len;

	p = 0u; append(f0, sizeof(f0), &p, "yeelight/"); append(f0, sizeof(f0), &p, g_muid_hex); append(f0, sizeof(f0), &p, "/set");
	p = 0u; append(f1, sizeof(f1), &p, "yeelight/"); append(f1, sizeof(f1), &p, g_muid_hex); append(f1, sizeof(f1), &p, "/ota/install");
	p = 0u; append(f2, sizeof(f2), &p, "yeelight/"); append(f2, sizeof(f2), &p, g_muid_hex); append(f2, sizeof(f2), &p, "/config/#");

	p = vh_start;
	wr_u16(g_tx_pkt, &p, 0x0001u);
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt) - 1u, &p, f0) != 0) return -1;
	g_tx_pkt[p++] = 0x00u;
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt) - 1u, &p, f1) != 0) return -1;
	g_tx_pkt[p++] = 0x00u;
	if (wr_utf8(g_tx_pkt, sizeof(g_tx_pkt) - 1u, &p, f2) != 0) return -1;
	g_tx_pkt[p++] = 0x00u;

	rem_len = p - vh_start;
	fh_len = 1u + encode_rem_len(rem_hdr, (uint32_t)rem_len);
	g_tx_pkt[0] = 0x82u;
	for (size_t i = 0u; i < fh_len - 1u; i++)
		g_tx_pkt[1u + i] = rem_hdr[i];
	if (fh_len != vh_start) {
		for (size_t i = 0u; i < rem_len; i++)
			g_tx_pkt[fh_len + i] = g_tx_pkt[vh_start + i];
	}
	g_tx_len = fh_len + rem_len;
	g_tx_sent = 0u;
	return flush_tx(now);
}

static int queue_pingreq(uint32_t now)
{
	g_tx_pkt[0] = 0xC0u;
	g_tx_pkt[1] = 0x00u;
	g_tx_len = 2u;
	g_tx_sent = 0u;
	g_stats.ping_tx++;
	g_ping_outstanding = 1u;
	g_ping_sent_ms = now;
	return flush_tx(now);
}

/* ------------------------------------------- HA 2026.11 Discovery & Telemetry --- */
static void append_device_json(char *out, size_t cap, size_t *pos)
{
	char ip[24];
	int rssi = 0;
	uint8_t slot = 0u;

	ip[0] = '\0';
	if (g_io && g_io->diag_info)
		(void)g_io->diag_info(ip, sizeof(ip), &rssi, &slot);

	append(out, cap, pos, ",\"dev\":{\"ids\":[\"yeelight_");
	append(out, cap, pos, g_muid_hex);
	append(out, cap, pos, "\"],\"name\":\"Yeelight ");
	append(out, cap, pos, g_muid_hex);
	append(out, cap, pos, "\",\"mdl\":\"YLDP06YL\",\"mf\":\"Yeelight (mcufw)\",\"sw\":\"");
	append(out, cap, pos, MCUFW_MQTT_FW_VERSION);
	append(out, cap, pos, "\"");
	if (ip[0] && !seq(ip, "0.0.0.0")) {
		append(out, cap, pos, ",\"cu\":\"http://");
		append(out, cap, pos, ip);
		append(out, cap, pos, ":8080/\"");
	}
	append(out, cap, pos, "}");
}

static void fmt_dev_topic(const char *sub)
{
	size_t p = 0u;
	append(g_topic_buf, sizeof(g_topic_buf), &p, "yeelight/");
	append(g_topic_buf, sizeof(g_topic_buf), &p, g_muid_hex);
	append(g_topic_buf, sizeof(g_topic_buf), &p, sub);
}

static void fmt_ha_topic(const char *comp, const char *suffix)
{
	size_t p = 0u;
	append(g_topic_buf, sizeof(g_topic_buf), &p, "homeassistant/");
	append(g_topic_buf, sizeof(g_topic_buf), &p, comp);
	append(g_topic_buf, sizeof(g_topic_buf), &p, "/yeelight_");
	append(g_topic_buf, sizeof(g_topic_buf), &p, g_muid_hex);
	append(g_topic_buf, sizeof(g_topic_buf), &p, suffix);
	append(g_topic_buf, sizeof(g_topic_buf), &p, "/config");
}

static void begin_ha_json(size_t *p, const char *name_json, const char *id_suffix,
                          const char *ent_cat)
{
	*p = 0u;
	append(g_json_buf, sizeof(g_json_buf), p, "{\"~\":\"yeelight/");
	append(g_json_buf, sizeof(g_json_buf), p, g_muid_hex);
	append(g_json_buf, sizeof(g_json_buf), p, "\",\"name\":");
	append(g_json_buf, sizeof(g_json_buf), p, name_json);
	append(g_json_buf, sizeof(g_json_buf), p, ",\"uniq_id\":\"yeelight_");
	append(g_json_buf, sizeof(g_json_buf), p, g_muid_hex);
	append(g_json_buf, sizeof(g_json_buf), p, id_suffix);
	append(g_json_buf, sizeof(g_json_buf), p, "\",\"avty_t\":\"~/availability\"");
	if (ent_cat && ent_cat[0]) {
		append(g_json_buf, sizeof(g_json_buf), p, ",\"ent_cat\":\"");
		append(g_json_buf, sizeof(g_json_buf), p, ent_cat);
		append(g_json_buf, sizeof(g_json_buf), p, "\"");
	}
}

static int publish_state_json(uint32_t now)
{
	const mcufw_state_t *st = mcufw_state();
	uint16_t mireds = st->ct ? (uint16_t)((1000000u + st->ct / 2u) / st->ct) : 250u;
	size_t p = 0u;

	if (mireds < 153u) mireds = 153u;
	if (mireds > 588u) mireds = 588u;

	fmt_dev_topic("/state");
	append(g_json_buf, sizeof(g_json_buf), &p, "{\"state\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, st->power ? "ON" : "OFF");
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"brightness\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->bright);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"color_mode\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p,
	       st->color_mode == MCUFW_COLOR_MODE_CT ? "color_temp" : "rgb");
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"color_temp\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, mireds);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"color\":{\"r\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->rgb_r);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"g\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->rgb_g);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"b\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->rgb_b);
	append(g_json_buf, sizeof(g_json_buf), &p, "},\"power\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, st->power ? "on" : "off");
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"bright\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->bright);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"ct\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->ct);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"hue\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->hue);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"sat\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, st->sat);
	append(g_json_buf, sizeof(g_json_buf), &p, "}");
	return queue_publish(g_topic_buf, g_json_buf, p, 0, now);
}

static int publish_ota_state_json(uint32_t now)
{
	const mcufw_ota_state_t *ota = mcufw_ota_state();
	int in_prog = (ota->status == MCUFW_OTA_QUEUED || ota->status == MCUFW_OTA_RUNNING);
	size_t p = 0u;

	fmt_dev_topic("/ota/state");
	append(g_json_buf, sizeof(g_json_buf), &p, "{\"installed_version\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, MCUFW_MQTT_FW_VERSION);
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"latest_version\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, MCUFW_MQTT_FW_VERSION);
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"in_progress\":");
	append(g_json_buf, sizeof(g_json_buf), &p, in_prog ? "true" : "false");
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"status\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, mcufw_ota_status_name(ota->status));
	append(g_json_buf, sizeof(g_json_buf), &p, "\"}");
	return queue_publish(g_topic_buf, g_json_buf, p, 1, now);
}

static int publish_cfg_state_json(uint32_t now)
{
	const mcufw_devcfg_t *cfg = mcufw_devcfg();
	size_t p = 0u;

	fmt_dev_topic("/config/state");
	append(g_json_buf, sizeof(g_json_buf), &p, "{\"power_on_mode\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p,
	       mcufw_devcfg_poweron_name(cfg->power_on_mode));
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"pwm_freq_hz\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, cfg->pwm_freq_hz);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"default_transition_ms\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, cfg->default_transition_ms);
	append(g_json_buf, sizeof(g_json_buf), &p, "}");
	return queue_publish(g_topic_buf, g_json_buf, p, 1, now);
}

static int publish_diag_json(uint32_t now)
{
	const mcufw_ota_state_t *ota = mcufw_ota_state();
	char ip[24];
	int rssi = 0;
	uint8_t slot = 0u;
	uint32_t up_s = (now - g_start_ms) / 1000u;
	size_t p = 0u;

	scpy(ip, sizeof(ip), "0.0.0.0");
	if (g_io && g_io->diag_info)
		(void)g_io->diag_info(ip, sizeof(ip), &rssi, &slot);

	fmt_dev_topic("/diag");
	append(g_json_buf, sizeof(g_json_buf), &p, "{\"rssi\":");
	append_sdec(g_json_buf, sizeof(g_json_buf), &p, rssi);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"ip\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, ip);
	append(g_json_buf, sizeof(g_json_buf), &p, "\",\"active_slot\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, slot);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"uptime_s\":");
	append_dec(g_json_buf, sizeof(g_json_buf), &p, up_s);
	append(g_json_buf, sizeof(g_json_buf), &p, ",\"ota_status\":\"");
	append(g_json_buf, sizeof(g_json_buf), &p, mcufw_ota_status_name(ota->status));
	append(g_json_buf, sizeof(g_json_buf), &p, "\"}");
	return queue_publish(g_topic_buf, g_json_buf, p, 0, now);
}

static int run_discovery_step(uint8_t step, uint32_t now)
{
	size_t p = 0u;

	switch (step) {
	case 0:
		fmt_dev_topic("/availability");
		return queue_publish(g_topic_buf, "online", 6u, 1, now);
	case 1:
		fmt_ha_topic("light", "");
		begin_ha_json(&p, "null", "_light", NULL);
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"schema\":\"json\",\"cmd_t\":\"~/set\",\"stat_t\":\"~/state\","
		       "\"brightness\":true,\"brightness_scale\":100,\"color_mode\":true,"
		       "\"supported_color_modes\":[\"color_temp\",\"rgb\"],"
		       "\"min_mireds\":153,\"max_mireds\":588");
		break;
	case 2:
		fmt_ha_topic("update", "_fw");
		begin_ha_json(&p, "\"Firmware\"", "_fw", NULL);
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"stat_t\":\"~/ota/state\",\"cmd_t\":\"~/ota/install\","
		       "\"dev_cla\":\"firmware\"");
		break;
	case 3:
		fmt_ha_topic("select", "_poweron");
		begin_ha_json(&p, "\"Power-On Mode\"", "_poweron", "config");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"stat_t\":\"~/config/state\",\"val_tpl\":\"{{ value_json.power_on_mode }}\","
		       "\"cmd_t\":\"~/config/power_on_mode/set\","
		       "\"options\":[\"last\",\"off\",\"on_default\"]");
		break;
	case 4:
		fmt_ha_topic("number", "_pwm_freq");
		begin_ha_json(&p, "\"PWM Frequency\"", "_pwm_freq", "config");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"stat_t\":\"~/config/state\",\"val_tpl\":\"{{ value_json.pwm_freq_hz }}\","
		       "\"cmd_t\":\"~/config/pwm_freq_hz/set\","
		       "\"min\":500,\"max\":20000,\"step\":1,\"unit_of_meas\":\"Hz\",\"mode\":\"box\"");
		break;
	case 5:
		fmt_ha_topic("number", "_transition");
		begin_ha_json(&p, "\"Default Transition\"", "_transition", "config");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"stat_t\":\"~/config/state\",\"val_tpl\":\"{{ value_json.default_transition_ms }}\","
		       "\"cmd_t\":\"~/config/transition_ms/set\","
		       "\"min\":0,\"max\":5000,\"step\":50,\"unit_of_meas\":\"ms\",\"mode\":\"box\"");
		break;
	case 6:
		fmt_ha_topic("button", "_restart");
		begin_ha_json(&p, "\"Restart\"", "_restart", "config");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"cmd_t\":\"~/config/restart\",\"payload_press\":\"PRESS\","
		       "\"dev_cla\":\"restart\"");
		break;
	case 7:
		fmt_ha_topic("button", "_rollback");
		begin_ha_json(&p, "\"Safe Rollback to Stock\"", "_rollback", "config");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"cmd_t\":\"~/config/rollback\",\"payload_press\":\"ROLLBACK\"");
		break;
	case 8:
		fmt_ha_topic("sensor", "_rssi");
		begin_ha_json(&p, "\"Wi-Fi Signal\"", "_rssi", "diagnostic");
		append(g_json_buf, sizeof(g_json_buf), &p,
		       ",\"stat_t\":\"~/diag\",\"val_tpl\":\"{{ value_json.rssi }}\","
		       "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\","
		       "\"json_attr_t\":\"~/diag\"");
		break;
	case 9:
		return publish_ota_state_json(now);
	case 10:
		return publish_cfg_state_json(now);
	case 11:
		return publish_diag_json(now);
	case 12:
		return publish_state_json(now);
	default:
		return 0;
	}

	append_device_json(g_json_buf, sizeof(g_json_buf), &p);
	append(g_json_buf, sizeof(g_json_buf), &p, "}");
	return queue_publish(g_topic_buf, g_json_buf, p, 1, now);
}

/* ------------------------------------------- Inbound PUBLISH dispatch --- */
static void handle_set_cmd(const char *payload, size_t plen)
{
	mcufw_state_t *st = mcufw_state();
	const char *csub = NULL;
	size_t csub_len = 0u;
	char str_v[24];
	long lval = 0;

	if (json_d1_obj(payload, plen, "color", &csub, &csub_len) == 0) {
		long r = 0, g = 0, b = 0;
		if (json_d1_long(csub, csub_len, "r", &r) == 0 &&
		    json_d1_long(csub, csub_len, "g", &g) == 0 &&
		    json_d1_long(csub, csub_len, "b", &b) == 0) {
			(void)mcufw_state_set_rgb(st, r, g, b);
		}
	} else if (json_d1_long(payload, plen, "color_temp", &lval) == 0 && lval > 0) {
		long k = (lval >= 1000L) ? lval : ((1000000L + lval / 2L) / lval);
		(void)mcufw_state_set_ct(st, k);
	} else if (json_d1_long(payload, plen, "ct", &lval) == 0 && lval > 0) {
		(void)mcufw_state_set_ct(st, lval);
	}

	if (json_d1_long(payload, plen, "brightness", &lval) == 0 ||
	    json_d1_long(payload, plen, "bright", &lval) == 0) {
		(void)mcufw_state_set_bright(st, lval);
	}

	if (json_d1_str(payload, plen, "state", str_v, sizeof(str_v)) == 0) {
		if (seq(str_v, "ON") || seq(str_v, "on"))
			(void)mcufw_state_set_power(st, 1);
		else if (seq(str_v, "OFF") || seq(str_v, "off"))
			(void)mcufw_state_set_power(st, 0);
	} else if (json_d1_str(payload, plen, "power", str_v, sizeof(str_v)) == 0) {
		if (seq(str_v, "on") || seq(str_v, "ON"))
			(void)mcufw_state_set_power(st, 1);
		else if (seq(str_v, "off") || seq(str_v, "OFF"))
			(void)mcufw_state_set_power(st, 0);
	}
}

static void handle_ota_cmd(const char *payload, size_t plen)
{
	char url[MCUFW_OTA_URL_MAX];
	char md5[MCUFW_OTA_MD5_HEX + 4u];

	url[0] = '\0';
	md5[0] = '\0';
	if (json_d1_str(payload, plen, "url", url, sizeof(url)) == 0 ||
	    json_d1_str(payload, plen, "app_url", url, sizeof(url)) == 0) {
		(void)json_d1_str(payload, plen, "md5", md5, sizeof(md5));
		if (!md5[0])
			(void)json_d1_str(payload, plen, "file_md5", md5, sizeof(md5));
	} else {
		trim_copy(payload, plen, url, sizeof(url));
	}
	if (url[0])
		(void)mcufw_ota_queue(url, md5[0] ? md5 : NULL);
}

static void handle_inbound_publish(const char *topic, const char *payload, size_t plen)
{
	char val[40];
	long lval = 0;

	g_stats.pub_rx++;
	fmt_dev_topic("/set");
	if (seq(topic, g_topic_buf)) {
		handle_set_cmd(payload, plen);
		return;
	}
	if (s_ends_with(topic, "/ota/install")) {
		handle_ota_cmd(payload, plen);
		return;
	}
	if (s_ends_with(topic, "/config/power_on_mode/set")) {
		trim_copy(payload, plen, val, sizeof(val));
		if (seq(val, "last"))
			(void)mcufw_devcfg_set_power_on_mode(MCUFW_POWERON_LAST);
		else if (seq(val, "off"))
			(void)mcufw_devcfg_set_power_on_mode(MCUFW_POWERON_OFF);
		else if (seq(val, "on_default"))
			(void)mcufw_devcfg_set_power_on_mode(MCUFW_POWERON_DEFAULT);
		g_config_dirty = 1u;
		return;
	}
	if (s_ends_with(topic, "/config/pwm_freq_hz/set")) {
		if (parse_trimmed_long(payload, plen, &lval) == 0 &&
		    lval >= (long)MCUFW_PWM_FREQ_MIN && lval <= (long)MCUFW_PWM_FREQ_MAX) {
			(void)mcufw_devcfg_set_pwm_freq((uint16_t)lval);
			g_config_dirty = 1u;
		}
		return;
	}
	if (s_ends_with(topic, "/config/transition_ms/set")) {
		if (parse_trimmed_long(payload, plen, &lval) == 0 &&
		    lval >= 0L && lval <= (long)MCUFW_TRANS_MAX_MS) {
			(void)mcufw_devcfg_set_transition_ms((uint16_t)lval);
			g_config_dirty = 1u;
		}
		return;
	}
	if (s_ends_with(topic, "/config/rollback")) {
		trim_copy(payload, plen, val, sizeof(val));
		if (seq(val, "ROLLBACK") || seq(val, "PRESS"))
			mcufw_ota_request_rollback();
		return;
	}
	if (s_ends_with(topic, "/config/restart")) {
		trim_copy(payload, plen, val, sizeof(val));
		if (seq(val, "PRESS")) {
			g_stats.restart_pending = 1;
			g_restart_ticks = 5u;
		}
	}
}

/* ------------------------------------------- MQTT 3.1.1 RX decoder --- */
static int rx_process_packets(uint32_t now)
{
	while (g_rx_len >= 2u) {
		uint32_t rem_len = 0u;
		uint32_t mult = 1u;
		size_t pos = 1u;
		int complete_hdr = 0;

		while (pos < g_rx_len && pos <= 4u) {
			uint8_t b = g_rx_acc[pos++];
			rem_len += (uint32_t)(b & 0x7fu) * mult;
			if ((b & 0x80u) == 0u) {
				complete_hdr = 1;
				break;
			}
			mult *= 128u;
		}
		if (!complete_hdr) {
			if (g_rx_len > 5u && pos > 4u)
				return -1;
			break;
		}
		if (pos + rem_len > sizeof(g_rx_acc))
			return -1;
		if (g_rx_len < pos + rem_len)
			break;

		{
			uint8_t ptype = (uint8_t)((g_rx_acc[0] >> 4) & 0x0fu);
			uint8_t flags = (uint8_t)(g_rx_acc[0] & 0x0fu);
			const uint8_t *body = g_rx_acc + pos;
			size_t pkt_total = pos + (size_t)rem_len;

			g_stats.last_rx_ms = now;
			switch (ptype) {
			case 2: /* CONNACK */
				if (rem_len < 2u || body[1] != 0x00u)
					return -1;
				if (g_stats.state == MCUFW_MQTT_ST_WAIT_CONNACK) {
					g_stats.state = MCUFW_MQTT_ST_DISCOVERY_BURST;
					g_stats.connects++;
					g_stats.backoff_ms = 0u;
					g_burst_step = 0u;
					g_ping_outstanding = 0u;
				}
				break;
			case 3: /* PUBLISH */
				if (rem_len >= 2u) {
					uint16_t tlen = (uint16_t)(((uint16_t)body[0] << 8) | body[1]);
					uint8_t qos = (uint8_t)((flags >> 1) & 0x03u);
					size_t off = 2u + (size_t)tlen;
					if (qos > 0u)
						off += 2u;
					if (off <= rem_len) {
						char topic[96];
						size_t copy_n = tlen < sizeof(topic) - 1u ? tlen : sizeof(topic) - 1u;
						for (size_t i = 0u; i < copy_n; i++)
							topic[i] = (char)body[2u + i];
						topic[copy_n] = '\0';
						handle_inbound_publish(topic, (const char *)(body + off),
						                       (size_t)rem_len - off);
					}
				}
				break;
			case 9: /* SUBACK */
				g_stats.suback_rx++;
				if (g_stats.state == MCUFW_MQTT_ST_SUBSCRIBE) {
					g_stats.state = MCUFW_MQTT_ST_CONNECTED;
					g_last_diag_ms = now;
				}
				break;
			case 13: /* PINGRESP */
				g_stats.ping_rx++;
				g_ping_outstanding = 0u;
				break;
			default:
				break;
			}

			if (pkt_total < g_rx_len) {
				for (size_t i = 0u; i < g_rx_len - pkt_total; i++)
					g_rx_acc[i] = g_rx_acc[pkt_total + i];
			}
			g_rx_len -= pkt_total;
		}
	}
	return 0;
}

/* ------------------------------------------- Public client API --- */
void mcufw_mqtt_client_init(uint32_t muid)
{
	g_muid = muid ? muid : 0x05eb8034u;
	refresh_muid_hex();
	g_stats.state = g_io ? MCUFW_MQTT_ST_DISCONNECTED : MCUFW_MQTT_ST_DISABLED;
	g_stats.connects = 0u;
	g_stats.disconnects = 0u;
	g_stats.pub_tx = 0u;
	g_stats.pub_rx = 0u;
	g_stats.ping_tx = 0u;
	g_stats.ping_rx = 0u;
	g_stats.suback_rx = 0u;
	g_stats.backoff_ms = 0u;
	g_stats.last_rx_ms = 0u;
	g_stats.restart_pending = 0;

	g_phase_ms = 0u;
	g_backoff_start_ms = 0u;
	g_last_tx_ms = 0u;
	g_ping_sent_ms = 0u;
	g_last_diag_ms = 0u;
	g_start_ms = cur_ms();
	g_last_pub_mutations = mcufw_state()->mutations;
	g_last_ota_status = mcufw_ota_state()->status;
	g_burst_step = 0u;
	g_ping_outstanding = 0u;
	g_config_dirty = 0u;
	g_restart_ticks = 0u;
	g_tx_len = 0u;
	g_tx_sent = 0u;
	g_rx_len = 0u;
	g_active_host[0] = '\0';
	g_active_port = 0u;
	g_active_user[0] = '\0';
}

void mcufw_mqtt_client_bind(const mcufw_mqtt_io_t *io)
{
	g_io = io;
	if (!io) {
		g_tx_len = 0u;
		g_tx_sent = 0u;
		g_rx_len = 0u;
		g_ping_outstanding = 0u;
		g_stats.state = MCUFW_MQTT_ST_DISABLED;
		g_stats.restart_pending = 0;
		g_restart_ticks = 0u;
	} else {
		g_start_ms = cur_ms();
		if (g_stats.state == MCUFW_MQTT_ST_DISABLED)
			g_stats.state = MCUFW_MQTT_ST_DISCONNECTED;
	}
}

void mcufw_mqtt_client_tick(void)
{
	const mcufw_devcfg_t *cfg;
	uint32_t now;

	if (g_stats.restart_pending) {
		if (g_restart_ticks > 0u) {
			g_restart_ticks--;
		} else {
			g_stats.restart_pending = 0;
			mcufw_rollback_hal_reset();
			return;
		}
	}

	if (!g_io || !g_io->link_ready || !g_io->tcp_connect || !g_io->tcp_state ||
	    !g_io->tcp_send || !g_io->tcp_recv || !g_io->tcp_close) {
		return;
	}

	now = cur_ms();
	cfg = mcufw_devcfg();
	if (!cfg || !cfg->mqtt_enabled || cfg->mqtt_host[0] == '\0') {
		if (g_stats.state != MCUFW_MQTT_ST_DISABLED) {
			close_conn(MCUFW_MQTT_ST_DISABLED);
			g_stats.backoff_ms = 0u;
		}
		return;
	}

	if (!g_io->link_ready()) {
		if (g_stats.state != MCUFW_MQTT_ST_DISCONNECTED) {
			close_conn(MCUFW_MQTT_ST_DISCONNECTED);
			g_stats.backoff_ms = 0u;
		}
		return;
	}

	if (g_stats.state >= MCUFW_MQTT_ST_TCP_CONNECTING &&
	    g_stats.state != MCUFW_MQTT_ST_BACKOFF &&
	    (!seq(g_active_host, cfg->mqtt_host) || g_active_port != cfg->mqtt_port ||
	     !seq(g_active_user, cfg->mqtt_user))) {
		close_conn(MCUFW_MQTT_ST_DISCONNECTED);
		g_stats.backoff_ms = 0u;
	}

	if (g_stats.state == MCUFW_MQTT_ST_DISABLED)
		g_stats.state = MCUFW_MQTT_ST_DISCONNECTED;

	if (g_stats.state == MCUFW_MQTT_ST_BACKOFF) {
		if ((uint32_t)(now - g_backoff_start_ms) < g_stats.backoff_ms)
			return;
		g_stats.state = MCUFW_MQTT_ST_DISCONNECTED;
	}

	if (g_stats.state == MCUFW_MQTT_ST_DISCONNECTED) {
		uint16_t port = cfg->mqtt_port ? cfg->mqtt_port : (uint16_t)MCUFW_MQTT_PORT;
		scpy(g_active_host, sizeof(g_active_host), cfg->mqtt_host);
		g_active_port = port;
		scpy(g_active_user, sizeof(g_active_user), cfg->mqtt_user);
		g_phase_ms = now;
		if (g_io->tcp_connect(g_active_host, port) != 0) {
			enter_backoff(now);
			return;
		}
		g_stats.state = MCUFW_MQTT_ST_TCP_CONNECTING;
	}

	if (g_stats.state == MCUFW_MQTT_ST_TCP_CONNECTING) {
		int st = g_io->tcp_state();
		if (st == 0) {
			enter_backoff(now);
			return;
		}
		if (st == 1) {
			if ((uint32_t)(now - g_phase_ms) >= 5000u)
				enter_backoff(now);
			return;
		}
		if (queue_connect(now) != 0) {
			enter_backoff(now);
			return;
		}
		g_stats.state = MCUFW_MQTT_ST_WAIT_CONNACK;
		g_phase_ms = now;
	}

	if (g_io->tcp_state() != 2) {
		enter_backoff(now);
		return;
	}
	if (flush_tx(now) != 0) {
		enter_backoff(now);
		return;
	}

	if (g_rx_len < sizeof(g_rx_acc)) {
		int rn = g_io->tcp_recv(g_rx_acc + g_rx_len, sizeof(g_rx_acc) - g_rx_len);
		if (rn < 0) {
			enter_backoff(now);
			return;
		}
		g_rx_len += (size_t)rn;
	}
	if (rx_process_packets(now) != 0) {
		enter_backoff(now);
		return;
	}

	if (g_stats.state == MCUFW_MQTT_ST_WAIT_CONNACK) {
		if ((uint32_t)(now - g_phase_ms) >= 5000u)
			enter_backoff(now);
		return;
	}

	if (g_stats.state == MCUFW_MQTT_ST_SUBSCRIBE) {
		if ((uint32_t)(now - g_phase_ms) >= 5000u)
			enter_backoff(now);
		return;
	}

	if (g_tx_sent < g_tx_len)
		return;

	if (g_stats.state == MCUFW_MQTT_ST_DISCOVERY_BURST) {
		if (g_burst_step < MQTT_BURST_ITEMS) {
			if (run_discovery_step(g_burst_step, now) != 0) {
				enter_backoff(now);
				return;
			}
			g_burst_step++;
			if (g_burst_step == MQTT_BURST_ITEMS) {
				g_last_pub_mutations = mcufw_state()->mutations;
				g_last_ota_status = mcufw_ota_state()->status;
				g_config_dirty = 0u;
			}
			return;
		}
		if (queue_subscribe(now) != 0) {
			enter_backoff(now);
			return;
		}
		g_stats.state = MCUFW_MQTT_ST_SUBSCRIBE;
		g_phase_ms = now;
		return;
	}

	if (g_stats.state == MCUFW_MQTT_ST_CONNECTED) {
		const mcufw_state_t *st = mcufw_state();
		const mcufw_ota_state_t *ota = mcufw_ota_state();

		if (st->mutations != g_last_pub_mutations) {
			if (publish_state_json(now) != 0) {
				enter_backoff(now);
				return;
			}
			g_last_pub_mutations = st->mutations;
			return;
		}
		if (ota->status != g_last_ota_status) {
			if (publish_ota_state_json(now) != 0) {
				enter_backoff(now);
				return;
			}
			g_last_ota_status = ota->status;
			return;
		}
		if (g_config_dirty) {
			if (publish_cfg_state_json(now) != 0) {
				enter_backoff(now);
				return;
			}
			g_config_dirty = 0u;
			return;
		}
		if (g_ping_outstanding) {
			if ((uint32_t)(now - g_ping_sent_ms) >= 10000u) {
				enter_backoff(now);
				return;
			}
		} else if ((uint32_t)(now - g_last_tx_ms) >= (MCUFW_MQTT_KEEPALIVE_S * 1000u)) {
			if (queue_pingreq(now) != 0)
				enter_backoff(now);
			return;
		}
		if ((uint32_t)(now - g_last_diag_ms) >= 30000u) {
			if (publish_diag_json(now) != 0) {
				enter_backoff(now);
				return;
			}
			g_last_diag_ms = now;
			return;
		}
	}
}

const mcufw_mqtt_client_stats_t *mcufw_mqtt_client_stats(void)
{
	return &g_stats;
}
