/*
 * mcufw_miio.c -- miIO UDP channel: header, hello handshake, AES session,
 * method dispatch.  Pure byte-buffer logic (no socket, no RTOS).
 *
 * Wire format (32-byte header, little endian), cross-checked against the
 * colour firmware (`ota` handler 0x1f0097e8, miIO module table 0x00103098,
 * docs/06 §1.2/§2):
 *
 *   off  size  field
 *   0x00   2   magic 0x2131
 *   0x02   2   total datagram length (header + payload)
 *   0x04   4   unknown / zero
 *   0x08   4   did
 *   0x0c   4   stamp (unix seconds on real devices)
 *   0x10  16   MD5(header[0x00..0x10] || token)
 *   0x20   n   payload: AES-128-CBC(key = MD5(token), iv = MD5(key||token))
 *
 * A datagram of exactly 32 bytes is a `hello`.  On real hardware the reply
 * carries the *device token* in the position where the request left it zero;
 * an unpaired device answers with 16 x 0xff, which is precisely what the
 * `.115` test carrier does today (`did 05eb8034`, `token ffffffff…`, then the
 * encrypted request times out) -- see docs/06 §0 and docs/10 §2.
 */
#include "mcufw_crypto.h"
#include "mcufw_miio.h"
#include "mcufw_state.h"

/* ------------------------------------------------------------ byte utils -- */
static uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void cp(uint8_t *dst, const uint8_t *src, size_t n)
{
	while (n--)
		*dst++ = *src++;
}

static void zpad(uint8_t *dst, size_t n)
{
	while (n--)
		*dst++ = 0;
}

static int token_is_ff(const uint8_t token[MCUFW_MIIO_TOKEN_LEN])
{
	unsigned i;

	for (i = 0; i < MCUFW_MIIO_TOKEN_LEN; i++)
		if (token[i] != 0xffu)
			return 0;
	return 1;
}

/* ---------------------------------------------------------------- session - *
 * key = MD5(token); iv = MD5(key || token).  Both are derived per packet --
 * the token is the only long-term secret, Stamping/keystream reuse does not
 * exist because every request carries a fresh stamp.
 */
static void session_key_iv(const uint8_t token[MCUFW_MIIO_TOKEN_LEN],
			   uint8_t key[MCUFW_AES_BLOCK],
			   uint8_t iv[MCUFW_AES_BLOCK])
{
	mcufw_md5(token, MCUFW_MIIO_TOKEN_LEN, key);
	mcufw_md5_2(key, MCUFW_AES_BLOCK, token, MCUFW_MIIO_TOKEN_LEN, iv);
}

/* ------------------------------------------------------------- json parser -
 * A full JSON parser is not needed on this channel: the miIO request body is
 * always `{"id":<n>,"method":"<name>","params":…}`.  Two scanners cover it and
 * are immune to key order and to whitespace.
 */
static int json_find_string(const char *json, size_t len, const char *key,
			    char *out, size_t out_cap)
{
	size_t klen = 0;
	size_t i;

	while (key[klen])
		klen++;

	for (i = 0; i + klen + 2u < len; i++) {
		if (json[i] != '"')
			continue;
		{
			size_t j = 0;

			while (j < klen && json[i + 1u + j] == key[j])
				j++;
			if (j != klen || json[i + 1u + klen] != '"')
				continue;
		}
		/* skip  "key"  :  " */
		i += klen + 3u;
		while (i < len && (json[i] == ' ' || json[i] == '\t' ||
				   json[i] == '\r' || json[i] == '\n'))
			i++;
		if (i < len && json[i] == '"') {
			size_t n = 0;

			i++;
			while (i < len && json[i] != '"' && n + 1u < out_cap)
				out[n++] = json[i++];
			out[n] = '\0';
			return (int)n;
		}
		/* Non-string value (number/object): report "found, not a string". */
		return -1;
	}
	return -1;
}

static int json_find_int(const char *json, size_t len, const char *key,
			 int32_t *out)
{
	size_t klen = 0;
	size_t i;
	int neg = 0;
	int32_t v = 0;
	int digits = 0;

	while (key[klen])
		klen++;

	for (i = 0; i + klen + 2u < len; i++) {
		if (json[i] != '"')
			continue;
		{
			size_t j = 0;

			while (j < klen && json[i + 1u + j] == key[j])
				j++;
			if (j != klen || json[i + 1u + klen] != '"')
				continue;
		}
		i += klen + 3u;
		while (i < len && (json[i] == ' ' || json[i] == '\t' ||
				   json[i] == '\r' || json[i] == '\n'))
			i++;
		if (i < len && json[i] == '-') {
			neg = 1;
			i++;
		}
		while (i < len && json[i] >= '0' && json[i] <= '9') {
			v = (int32_t)(v * 10 + (json[i] - '0'));
			i++;
			digits++;
		}
		if (!digits)
			return -1;
		*out = neg ? -v : v;
		return 0;
	}
	return -1;
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void str_cpy(char *dst, const char *src, size_t cap)
{
	size_t n = 0;

	while (src[n] && n + 1u < cap) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
}

/* Does `json` contain the exact quoted token "tok"?  Used for the miIO
 * `set_ps` argument, which arrives as ["on"], "on" or {"power":"on"} -- all
 * three spell the state as a quoted token, so a token scan covers every
 * shape without a JSON parser. */
static int has_quoted(const char *json, size_t len, const char *tok)
{
	size_t tlen = 0;
	size_t i;

	while (tok[tlen])
		tlen++;

	for (i = 0; i + tlen + 1u < len; i++) {
		size_t j = 0;

		if (json[i] != '"')
			continue;
		while (j < tlen && json[i + 1u + j] == tok[j])
			j++;
		if (j == tlen && json[i + 1u + tlen] == '"')
			return 1;
	}
	return 0;
}

/* --------------------------------------------------------------- response - */
struct sbuf {
	uint8_t *p;
	size_t   cap;
	size_t   len;
	int      ovf;
};

static void sb_put(struct sbuf *b, const char *s)
{
	while (*s) {
		if (b->len + 1u > b->cap) {
			b->ovf = 1;
			return;
		}
		b->p[b->len++] = (uint8_t)*s++;
	}
}

static void sb_dec(struct sbuf *b, int32_t v)
{
	char tmp[12];
	int n = 0;
	uint32_t u;

	if (v < 0) {
		sb_put(b, "-");
		u = (uint32_t)(-(v + 1)) + 1u;
	} else {
		u = (uint32_t)v;
	}
	do {
		tmp[n++] = (char)('0' + (u % 10u));
		u /= 10u;
	} while (u && n < (int)sizeof(tmp));
	while (n--)
		sb_put(b, (const char[]){ tmp[n], 0 });
}

/* ---------------------------------------------------------------- hello --- */
static int handle_hello(mcufw_miio_ctx_t *ctx, const uint8_t *in,
			uint8_t *out, size_t out_cap, size_t *out_len)
{
	uint8_t digest[MCUFW_MD5_LEN];

	if (out_cap < MCUFW_MIIO_HDR_LEN)
		return MCUFW_MIIO_E_ROOM;

	/* Replay guard: a hello must carry a stamp that moves forward.  A real
	 * device silently drops stale hellos; we do the same but count it. */
	if (ctx->stamp != 0u && rd_u32(in + 12) <= ctx->stamp) {
		ctx->rejected++;
		*out_len = 0;
		return MCUFW_MIIO_OK;
	}
	ctx->stamp = rd_u32(in + 12);

	out[0] = (uint8_t)(MCUFW_MIIO_MAGIC);
	out[1] = (uint8_t)(MCUFW_MIIO_MAGIC >> 8);
	wr_u16(out + 2, (uint16_t)MCUFW_MIIO_HDR_LEN);
	wr_u32(out + 4, 0u);
	wr_u32(out + 8, ctx->did);
	wr_u32(out + 12, ctx->stamp);
	zpad(out + 16, MCUFW_MIIO_TOKEN_LEN);
	cp(out + 16, ctx->token, MCUFW_MIIO_TOKEN_LEN);

	/* checksum = MD5(header[0..16) || token) */
	mcufw_md5_2(out, 16u, ctx->token, MCUFW_MIIO_TOKEN_LEN, digest);
	cp(out + 16, digest, MCUFW_MD5_LEN);

	*out_len = MCUFW_MIIO_HDR_LEN;
	return MCUFW_MIIO_OK;
}

/* ------------------------------------------------------ encrypted request -- */
static int handle_encrypted(mcufw_miio_ctx_t *ctx, const uint8_t *in,
			    size_t in_len, uint8_t *out, size_t out_cap,
			    size_t *out_len)
{
	uint8_t key[MCUFW_AES_BLOCK], iv[MCUFW_AES_BLOCK];
	uint8_t digest[MCUFW_MD5_LEN];
	uint8_t plain[MCUFW_MIIO_MAX_PKT];
	char    method[MCUFW_MIIO_METHOD_MAX + 1u];
	char    json[MCUFW_MIIO_MAX_PKT];
	struct sbuf sb;
	int32_t id = 0;
	size_t clen = in_len - MCUFW_MIIO_HDR_LEN;
	size_t plen;

	if (!ctx->have_token) {
		/* Paired? No.  The reference lamp does exactly this: hello is
		 * answered with ff..ff and every encrypted request is dropped. */
		ctx->rejected++;
		*out_len = 0;
		return MCUFW_MIIO_E_TOKEN_MISSING;
	}

	mcufw_md5_2(in, 16u, ctx->token, MCUFW_MIIO_TOKEN_LEN, digest);
	{
		unsigned i;

		for (i = 0; i < MCUFW_MD5_LEN; i++)
			if (digest[i] != in[16 + i]) {
				ctx->rejected++;
				*out_len = 0;
				return MCUFW_MIIO_E_CHECKSUM;
			}
	}

	if (clen == 0 || clen > sizeof(plain)) {
		ctx->rejected++;
		*out_len = 0;
		return MCUFW_MIIO_E_LEN;
	}

	session_key_iv(ctx->token, key, iv);
	plen = mcufw_aes128_cbc_decrypt(key, iv, in + MCUFW_MIIO_HDR_LEN, clen,
					plain);
	if (plen == 0 || plen + 1u > sizeof(json)) {
		ctx->rejected++;
		*out_len = 0;
		return MCUFW_MIIO_E_CRYPTO;
	}
	cp((uint8_t *)json, plain, plen);
	json[plen] = '\0';

	method[0] = '\0';
	if (json_find_string(json, plen, "method", method, sizeof(method)) < 0) {
		ctx->rejected++;
		*out_len = 0;
		return MCUFW_MIIO_E_JSON;
	}
	(void)json_find_int(json, plen, "id", &id);
	str_cpy(ctx->last_method, method, sizeof(ctx->last_method));

	/* Response JSON: {"id":<id>,"result":[…]} or the JSON-RPC error shape. */
	{
		uint8_t rsp[MCUFW_MIIO_MAX_PKT];
		size_t rsp_len;

		sb.p = rsp;
		sb.cap = sizeof(rsp) - 1u;
		sb.len = 0;
		sb.ovf = 0;

		sb_put(&sb, "{\"id\":");
		sb_dec(&sb, id);

		if (str_eq(method, "miIO.ota")) {
			/* Only OTA channel that exists at all (docs/06 §1.2).
			 * 3.4 answers the RPC; the download itself (app_url,
			 * file_md5, proc=dnld) is phase 5. */
			ctx->ota_requests++;
			sb_put(&sb, ",\"result\":[\"ok\"]}");
		} else if (str_eq(method, "miIO.info")) {
			/* Live state, not a static string: a controller that
			 * polls miIO.info after a set_ps sees the mutation. */
			const mcufw_state_t *st = mcufw_state();

			sb_put(&sb, ",\"result\":{\"model\":\"mcufw\","
				   "\"fw_ver\":\"0.6-phase3.6\",\"power\":\"");
			sb_put(&sb, st->power ? "on" : "off");
			sb_put(&sb, "\",\"bright\":");
			sb_dec(&sb, (int32_t)st->bright);
			sb_put(&sb, ",\"ct\":");
			sb_dec(&sb, (int32_t)st->ct);
			sb_put(&sb, ",\"hue\":");
			sb_dec(&sb, (int32_t)st->hue);
			sb_put(&sb, ",\"sat\":");
			sb_dec(&sb, (int32_t)st->sat);
			sb_put(&sb, ",\"color_mode\":");
			sb_dec(&sb, (int32_t)st->color_mode);
			sb_put(&sb, ",\"cf\":");
			sb_dec(&sb, (int32_t)st->cf_active);
			sb_put(&sb, ",\"name\":\"");
			sb_put(&sb, st->name);
			sb_put(&sb, "\"}}");
		} else if (str_eq(method, "set_ps")) {
			/* set_ps is blocked on LAN 55443 and only reachable
			 * here (UDP 54321) -- that is the whole point of the
			 * split whitelist.  Mutates the shared lamp state. */
			mcufw_state_t *st = mcufw_state();

			if (has_quoted(json, plen, "on")) {
				mcufw_state_set_power(st, 1);
				sb_put(&sb, ",\"result\":[\"ok\"]}");
			} else if (has_quoted(json, plen, "off")) {
				mcufw_state_set_power(st, 0);
				sb_put(&sb, ",\"result\":[\"ok\"]}");
			} else {
				sb_put(&sb, ",\"error\":{\"code\":-32602,"
					   "\"message\":\"invalid params\"}}");
			}
		} else if (str_eq(method, "get_ps")) {
			const mcufw_state_t *st = mcufw_state();

			sb_put(&sb, ",\"result\":[{\"power\":\"");
			sb_put(&sb, st->power ? "on" : "off");
			sb_put(&sb, "\",\"bright\":");
			sb_dec(&sb, (int32_t)st->bright);
			sb_put(&sb, ",\"ct\":");
			sb_dec(&sb, (int32_t)st->ct);
			sb_put(&sb, ",\"hue\":");
			sb_dec(&sb, (int32_t)st->hue);
			sb_put(&sb, ",\"sat\":");
			sb_dec(&sb, (int32_t)st->sat);
			sb_put(&sb, ",\"color_mode\":");
			sb_dec(&sb, (int32_t)st->color_mode);
			sb_put(&sb, ",\"name\":\"");
			sb_put(&sb, st->name);
			sb_put(&sb, "\"}]}");
		} else {
			sb_put(&sb, ",\"error\":{\"code\":-32601,\"message\":\"method not supported\"}}");
		}

		if (sb.ovf)
			return MCUFW_MIIO_E_ROOM;
		rsp_len = sb.len;

		/* header + AES-CBC(padded) response */
		if (out_cap < MCUFW_MIIO_HDR_LEN + rsp_len + MCUFW_AES_BLOCK)
			return MCUFW_MIIO_E_ROOM;

		out[0] = (uint8_t)(MCUFW_MIIO_MAGIC);
		out[1] = (uint8_t)(MCUFW_MIIO_MAGIC >> 8);
		wr_u32(out + 4, 0u);
		wr_u32(out + 8, ctx->did);
		wr_u32(out + 12, ctx->stamp);

		{
			size_t out_plen = mcufw_aes128_cbc_encrypt(
				key, iv, rsp, rsp_len, out + MCUFW_MIIO_HDR_LEN);

			wr_u16(out + 2,
			       (uint16_t)(MCUFW_MIIO_HDR_LEN + out_plen));
			mcufw_md5_2(out, 16u, ctx->token,
				    MCUFW_MIIO_TOKEN_LEN, digest);
			cp(out + 16, digest, MCUFW_MD5_LEN);
			*out_len = MCUFW_MIIO_HDR_LEN + out_plen;
		}
	}

	return MCUFW_MIIO_OK;
}

/* ----------------------------------------------------------------- entry -- */
void mcufw_miio_ctx_init(mcufw_miio_ctx_t *ctx, uint32_t did)
{
	unsigned i;

	ctx->did = did;
	ctx->stamp = 0;
	ctx->have_token = 0;
	ctx->ota_requests = 0;
	ctx->rejected = 0;
	ctx->last_method[0] = '\0';
	for (i = 0; i < MCUFW_MIIO_TOKEN_LEN; i++)
		ctx->token[i] = 0xffu;
}

int mcufw_miio_set_token(mcufw_miio_ctx_t *ctx,
			 const uint8_t token[MCUFW_MIIO_TOKEN_LEN])
{
	cp(ctx->token, token, MCUFW_MIIO_TOKEN_LEN);
	ctx->have_token = (uint8_t)!token_is_ff(token);
	return ctx->have_token ? MCUFW_MIIO_OK : MCUFW_MIIO_E_TOKEN_MISSING;
}

int mcufw_miio_handle(mcufw_miio_ctx_t *ctx,
		      const uint8_t *in, size_t in_len,
		      uint8_t *out, size_t out_cap, size_t *out_len)
{
	uint16_t magic;
	uint16_t declared;

	*out_len = 0;

	if (in_len < MCUFW_MIIO_HDR_LEN)
		return MCUFW_MIIO_E_SHORT;

	magic = rd_u16(in);
	if (magic != MCUFW_MIIO_MAGIC)
		return MCUFW_MIIO_E_MAGIC;

	declared = rd_u16(in + 2);
	if (declared != (uint16_t)in_len || in_len > MCUFW_MIIO_MAX_PKT)
		return MCUFW_MIIO_E_LEN;

	if (in_len == MCUFW_MIIO_HDR_LEN)
		return handle_hello(ctx, in, out, out_cap, out_len);

	return handle_encrypted(ctx, in, in_len, out, out_cap, out_len);
}

const char *mcufw_miio_strerror(int rc)
{
	switch (rc) {
	case MCUFW_MIIO_OK:              return "ok";
	case MCUFW_MIIO_E_SHORT:         return "short";
	case MCUFW_MIIO_E_MAGIC:         return "bad-magic";
	case MCUFW_MIIO_E_LEN:           return "bad-length";
	case MCUFW_MIIO_E_CHECKSUM:      return "bad-checksum";
	case MCUFW_MIIO_E_TOKEN_MISSING: return "no-token";
	case MCUFW_MIIO_E_CRYPTO:        return "decrypt-failed";
	case MCUFW_MIIO_E_JSON:          return "no-method";
	case MCUFW_MIIO_E_UNSUPPORTED:   return "unsupported";
	case MCUFW_MIIO_E_ROOM:          return "buffer-too-small";
	default:                          return "?";
	}
}
