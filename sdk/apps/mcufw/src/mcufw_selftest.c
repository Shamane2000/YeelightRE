/*
 * mcufw_selftest.c -- known-answer + round-trip tests for the miIO channel.
 *
 * Vectors are the published ones, so a PASS is falsifiable:
 *   - MD5:      RFC 1321 appendix A.5 ("" and "abc")
 *   - AES-128:  FIPS-197 appendix B / C.1 (single block)
 *   - AES-CBC:  NIST SP 800-38A F.2.1, first block
 * The miIO cases drive mcufw_miio_handle() with packets built by the *same*
 * crypto code that encrypts the reply, plus hand-built negative cases (bad
 * magic, bad checksum, stale stamp, truncated datagram).
 */
#include "mcufw_crypto.h"
#include "mcufw_miio.h"
#include "mcufw_selftest.h"

static const char *const case_names[] = {
	"md5-empty",        /* RFC 1321 A.5          */
	"md5-abc",          /* RFC 1321 A.5          */
	"aes-block",        /* FIPS-197 C.1          */
	"aes-cbc-nist",     /* SP 800-38A F.2.1      */
	"aes-cbc-roundtrip",
	"miio-hello-reply",
	"miio-token-detect",
	"miio-ota-roundtrip",
	"miio-unknown-method",
	"miio-bad-magic",
	"miio-bad-checksum",
	"miio-stale-hello",
	"miio-short-packet",
};

#define CASE_COUNT (sizeof(case_names) / sizeof(case_names[0]))

unsigned mcufw_selftest_count(void)
{
	return (unsigned)CASE_COUNT;
}

const char *mcufw_selftest_name(unsigned bit)
{
	return bit < CASE_COUNT ? case_names[bit] : "?";
}

/* ------------------------------------------------------------- utilities -- */
static int hexeq(const uint8_t *got, const char *want, unsigned n)
{
	static const char hex[] = "0123456789abcdef";
	unsigned i;

	for (i = 0; i < n; i++) {
		unsigned hi = (unsigned)(got[i] >> 4);
		unsigned lo = (unsigned)(got[i] & 15u);

		if (hex[hi] != want[i * 2u] || hex[lo] != want[i * 2u + 1u])
			return 0;
	}
	return 1;
}

static void from_hex(uint8_t *dst, const char *src, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		unsigned v = 0;

		for (unsigned k = 0; k < 2u; k++) {
			char c = src[i * 2u + k];

			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= (unsigned)(c - 'A' + 10);
		}
		dst[i] = (uint8_t)v;
	}
}

static int contains(const uint8_t *hay, size_t hay_len, const char *needle)
{
	size_t nlen = 0;

	while (needle[nlen])
		nlen++;
	if (nlen == 0 || nlen > hay_len)
		return 0;
	for (size_t i = 0; i + nlen <= hay_len; i++) {
		size_t j = 0;

		while (j < nlen && hay[i + j] == (uint8_t)needle[j])
			j++;
		if (j == nlen)
			return 1;
	}
	return 0;
}

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void derive(const uint8_t token[16], uint8_t key[16], uint8_t iv[16])
{
	mcufw_md5(token, 16u, key);
	mcufw_md5_2(key, 16u, token, 16u, iv);
}

/* Build an encrypted miIO request packet; returns the datagram length. */
static size_t build_request(uint8_t *pkt, uint32_t did, uint32_t stamp,
			    const uint8_t token[16], const char *json)
{
	uint8_t key[16], iv[16], digest[16];
	size_t jlen = 0;
	size_t clen;

	while (json[jlen])
		jlen++;

	derive(token, key, iv);
	clen = mcufw_aes128_cbc_encrypt(key, iv, (const uint8_t *)json, jlen,
					pkt + MCUFW_MIIO_HDR_LEN);

	pkt[0] = 0x31;
	pkt[1] = 0x21;
	wr16(pkt + 2, (uint16_t)(MCUFW_MIIO_HDR_LEN + clen));
	wr32(pkt + 4, 0u);
	wr32(pkt + 8, did);
	wr32(pkt + 12, stamp);
	mcufw_md5_2(pkt, 16u, token, 16u, digest);
	for (unsigned i = 0; i < 16u; i++)
		pkt[16 + i] = digest[i];

	return MCUFW_MIIO_HDR_LEN + clen;
}

/* Decrypt a reply payload in place into `out`; returns plain length or 0. */
static size_t open_reply(const uint8_t *pkt, size_t pkt_len,
			 const uint8_t token[16], uint8_t *out)
{
	uint8_t key[16], iv[16];

	if (pkt_len <= MCUFW_MIIO_HDR_LEN)
		return 0;
	derive(token, key, iv);
	return mcufw_aes128_cbc_decrypt(key, iv, pkt + MCUFW_MIIO_HDR_LEN,
					pkt_len - MCUFW_MIIO_HDR_LEN, out);
}

static int reply_header_ok(const uint8_t *pkt, size_t pkt_len,
			   const uint8_t token[16], uint16_t declared_len)
{
	uint8_t digest[16];

	if (pkt_len < MCUFW_MIIO_HDR_LEN)
		return 0;
	if (pkt[0] != 0x31 || pkt[1] != 0x21)
		return 0;
	if ((size_t)((unsigned)pkt[2] | ((unsigned)pkt[3] << 8)) != pkt_len)
		return 0;
	if (declared_len != 0 && declared_len != (uint16_t)pkt_len)
		return 0;

	mcufw_md5_2(pkt, 16u, token, 16u, digest);
	for (unsigned i = 0; i < 16u; i++)
		if (digest[i] != pkt[16 + i])
			return 0;
	return 1;
}

/* ------------------------------------------------------------------ cases -- */
static int case_md5(void)
{
	uint8_t d[16];

	mcufw_md5((const uint8_t *)"", 0, d);
	if (!hexeq(d, "d41d8cd98f00b204e9800998ecf8427e", 16))
		return 0;
	mcufw_md5((const uint8_t *)"abc", 3, d);
	return hexeq(d, "900150983cd24fb0d6963f7d28e17f72", 16);
}

static int case_md5_abc(void)
{
	uint8_t d[16];

	mcufw_md5((const uint8_t *)"abc", 3, d);
	return hexeq(d, "900150983cd24fb0d6963f7d28e17f72", 16);
}

static int case_aes_block(void)
{
	uint8_t key[16], pt[16], ct[16], back[16];
	mcufw_aes128_t ctx;

	from_hex(key, "000102030405060708090a0b0c0d0e0f", 16);
	from_hex(pt, "00112233445566778899aabbccddeeff", 16);

	mcufw_aes128_init(&ctx, key);
	mcufw_aes128_encrypt_block(&ctx, pt, ct);
	if (!hexeq(ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16))
		return 0;

	mcufw_aes128_decrypt_block(&ctx, ct, back);
	return hexeq(back, "00112233445566778899aabbccddeeff", 16);
}

static int case_aes_cbc_nist(void)
{
	uint8_t key[16], iv[16], pt[16], ct[32];
	mcufw_aes128_t ctx;

	from_hex(key, "2b7e151628aed2a6abf7158809cf4f3c", 16);
	from_hex(iv, "000102030405060708090a0b0c0d0e0f", 16);
	from_hex(pt, "6bc1bee22e409f96e93d7e117393172a", 16);

	/* 16 B input => 16 B PKCS#7 pad => 32 B output; block 0 is the vector. */
	if (mcufw_aes128_cbc_encrypt(key, iv, pt, 16u, ct) != 32u)
		return 0;
	if (!hexeq(ct, "7649abac8119b246cee98e9b12e9197d", 16))
		return 0;

	(void)ctx;
	return 1;
}

static int case_cbc_roundtrip(void)
{
	uint8_t key[16], iv[16], ct[64], pt[64], out[64];
	const char *msg = "0123456789abcdef";       /* exactly one block  */
	size_t clen, plen;

	from_hex(key, "2b7e151628aed2a6abf7158809cf4f3c", 16);
	from_hex(iv, "000102030405060708090a0b0c0d0e0f", 16);

	for (unsigned i = 0; i < 16u; i++)
		pt[i] = (uint8_t)msg[i];

	clen = mcufw_aes128_cbc_encrypt(key, iv, pt, 16u, ct);
	plen = mcufw_aes128_cbc_decrypt(key, iv, ct, clen, out);
	if (plen != 16u)
		return 0;
	for (unsigned i = 0; i < 16u; i++)
		if (out[i] != pt[i])
			return 0;

	/* Corrupt padding => decrypt must refuse. */
	ct[clen - 1] ^= 0xffu;
	return mcufw_aes128_cbc_decrypt(key, iv, ct, clen, out) == 0;
}

static int case_hello(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t pkt[64], out[64];
	size_t out_len = 0;
	uint8_t digest[16];

	mcufw_miio_ctx_init(&ctx, 0x05eb8034u);

	/* hello request: 32-byte header, all-zero body, nothing else. */
	pkt[0] = 0x31;
	pkt[1] = 0x21;
	wr16(pkt + 2, 32u);
	wr32(pkt + 4, 0u);
	wr32(pkt + 8, 0x05eb8034u);
	wr32(pkt + 12, 0x60000000u);
	for (unsigned i = 16; i < 32u; i++)
		pkt[i] = 0;

	if (mcufw_miio_handle(&ctx, pkt, 32u, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_OK)
		return 0;
	if (out_len != 32u)
		return 0;
	/* Reply layout is header(16) || MD5(header[0..16) || token) -- the token
	 * itself is NOT on the wire in a hello reply; the digest proves both sides
	 * know it.  Check the digest against an independent recomputation. */
	mcufw_md5_2(out, 16u, ctx.token, 16u, digest);
	for (unsigned i = 0; i < 16u; i++)
		if (out[16 + i] != digest[i])
			return 0;
	return 1;
}

static int case_token_detect(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t ff[16], tok[16];

	for (unsigned i = 0; i < 16u; i++) {
		ff[i] = 0xffu;
		tok[i] = (uint8_t)(i + 1u);
	}

	mcufw_miio_ctx_init(&ctx, 1u);
	if (ctx.have_token)
		return 0;
	if (mcufw_miio_set_token(&ctx, ff) != MCUFW_MIIO_E_TOKEN_MISSING)
		return 0;
	if (ctx.have_token)
		return 0;
	if (mcufw_miio_set_token(&ctx, tok) != MCUFW_MIIO_OK)
		return 0;
	return ctx.have_token == 1u;
}

static int case_ota(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t tok[16], pkt[512], out[512], plain[512];
	size_t out_len = 0, pkt_len, plen;

	for (unsigned i = 0; i < 16u; i++)
		tok[i] = (uint8_t)(0x10u + i);

	mcufw_miio_ctx_init(&ctx, 0x05eb8034u);
	if (mcufw_miio_set_token(&ctx, tok) != MCUFW_MIIO_OK)
		return 0;

	pkt_len = build_request(
		pkt, 0x05eb8034u, 0x60000010u, tok,
		"{\"id\":7,\"method\":\"miIO.ota\",\"params\":{\"mode\":\"normal\","
		"\"app_url\":\"http://192.168.1.10/fw.bin\",\"file_md5\":"
		"\"00000000000000000000000000000000\",\"proc\":\"dnld\","
		"\"did\":\"05eb8034\"}}");

	if (mcufw_miio_handle(&ctx, pkt, pkt_len, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_OK)
		return 0;
	if (!reply_header_ok(out, out_len, tok, 0))
		return 0;
	plen = open_reply(out, out_len, tok, plain);
	if (plen == 0)
		return 0;
	if (!contains(plain, plen, "\"id\":7"))
		return 0;
	if (!contains(plain, plen, "\"result\":[\"ok\"]"))
		return 0;
	if (ctx.ota_requests != 1u)
		return 0;
	if (!contains((const uint8_t *)ctx.last_method, 9u, "miIO.ota"))
		return 0;
	return 1;
}

static int case_unknown(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t tok[16], pkt[256], out[256], plain[256];
	size_t out_len = 0, pkt_len, plen;

	for (unsigned i = 0; i < 16u; i++)
		tok[i] = (uint8_t)(0x20u + i);
	mcufw_miio_ctx_init(&ctx, 0x05eb8034u);
	if (mcufw_miio_set_token(&ctx, tok) != MCUFW_MIIO_OK)
		return 0;

	pkt_len = build_request(pkt, 0x05eb8034u, 0x60000020u, tok,
				"{\"id\":1,\"method\":\"get_prop\",\"params\":[]}");
	if (mcufw_miio_handle(&ctx, pkt, pkt_len, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_OK)
		return 0;
	plen = open_reply(out, out_len, tok, plain);
	if (plen == 0)
		return 0;
	return contains(plain, plen, "-32601");
}

static int case_bad_magic(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t tok[16], pkt[64], out[64];
	size_t out_len = 0;

	for (unsigned i = 0; i < 16u; i++)
		tok[i] = (uint8_t)i;
	mcufw_miio_ctx_init(&ctx, 1u);
	(void)mcufw_miio_set_token(&ctx, tok);

	pkt[0] = 0x99;
	pkt[1] = 0x21;
	wr16(pkt + 2, 32u);
	if (mcufw_miio_handle(&ctx, pkt, 32u, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_E_MAGIC)
		return 0;
	return out_len == 0;
}

static int case_bad_checksum(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t tok[16], pkt[256], out[256];
	size_t out_len = 0, pkt_len;

	for (unsigned i = 0; i < 16u; i++)
		tok[i] = (uint8_t)(0x30u + i);
	mcufw_miio_ctx_init(&ctx, 1u);
	if (mcufw_miio_set_token(&ctx, tok) != MCUFW_MIIO_OK)
		return 0;

	pkt_len = build_request(pkt, 1u, 0x60000100u, tok,
				"{\"id\":2,\"method\":\"miIO.info\"}");
	pkt[20] ^= 0x01u;                       /* flip a checksum byte */
	if (mcufw_miio_handle(&ctx, pkt, pkt_len, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_E_CHECKSUM)
		return 0;
	return out_len == 0;
}

static int case_stale_hello(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t pkt[64], out[64];
	size_t out_len = 0;

	mcufw_miio_ctx_init(&ctx, 1u);
	pkt[0] = 0x31;
	pkt[1] = 0x21;
	wr16(pkt + 2, 32u);
	wr32(pkt + 4, 0u);
	wr32(pkt + 8, 1u);
	wr32(pkt + 12, 1000u);
	for (unsigned i = 16; i < 32u; i++)
		pkt[i] = 0;

	if (mcufw_miio_handle(&ctx, pkt, 32u, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_OK || out_len != 32u)
		return 0;

	/* same stamp again => must be dropped (replay guard) */
	out_len = 0;
	if (mcufw_miio_handle(&ctx, pkt, 32u, out, sizeof(out), &out_len) !=
	    MCUFW_MIIO_OK)
		return 0;
	if (out_len != 0 || ctx.rejected != 1u)
		return 0;

	/* monotonic stamp => accepted again */
	wr32(pkt + 12, 1001u);
	return mcufw_miio_handle(&ctx, pkt, 32u, out, sizeof(out), &out_len) ==
		MCUFW_MIIO_OK && out_len == 32u;
}

static int case_short(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t pkt[8] = { 0x31, 0x21, 0, 0, 0, 0, 0, 0 };
	uint8_t out[32];
	size_t out_len = 0;

	mcufw_miio_ctx_init(&ctx, 1u);
	return mcufw_miio_handle(&ctx, pkt, sizeof(pkt), out, sizeof(out),
				 &out_len) == MCUFW_MIIO_E_SHORT && out_len == 0;
}

/* ------------------------------------------------------------------ entry -- */
uint32_t mcufw_selftest(void)
{
	uint32_t fail = 0;

	if (!case_md5())            fail |= 1u << 0;
	if (!case_md5_abc())        fail |= 1u << 1;
	if (!case_aes_block())      fail |= 1u << 2;
	if (!case_aes_cbc_nist())   fail |= 1u << 3;
	if (!case_cbc_roundtrip())  fail |= 1u << 4;
	if (!case_hello())          fail |= 1u << 5;
	if (!case_token_detect())   fail |= 1u << 6;
	if (!case_ota())            fail |= 1u << 7;
	if (!case_unknown())        fail |= 1u << 8;
	if (!case_bad_magic())      fail |= 1u << 9;
	if (!case_bad_checksum())   fail |= 1u << 10;
	if (!case_stale_hello())    fail |= 1u << 11;
	if (!case_short())          fail |= 1u << 12;

	return fail;
}
