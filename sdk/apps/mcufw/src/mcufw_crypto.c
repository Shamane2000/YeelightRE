/*
 * mcufw_crypto.c -- MD5 (RFC 1321) + AES-128-CBC (FIPS-197) in portable C11.
 *
 * No libc, no vendor archive, no malloc: everything lives on the stack or in
 * caller-provided buffers, so the code builds identically for the 88MW300
 * (arm-none-eabi, -ffreestanding -nostdlib) and for the host self-test.
 */
#include "mcufw_crypto.h"

/* ---------------------------------------------------------------- helpers -- */
static void mcufw_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	while (n--)
		*dst++ = *src++;
}

static void mcufw_zero(uint8_t *dst, size_t n)
{
	while (n--)
		*dst++ = 0;
}

/* ------------------------------------------------------------------- MD5 --- */
typedef mcufw_md5_ctx_t md5_ctx_t;

static const uint32_t md5_k[64] = {
	0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
	0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
	0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
	0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
	0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
	0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
	0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
	0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
	0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
	0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
	0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
	0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
	0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
	0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
	0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
	0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint8_t md5_s[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t rotl32(uint32_t x, unsigned c)
{
	return (x << c) | (x >> (32u - c));
}

static void md5_block(md5_ctx_t *c, const uint8_t *p)
{
	uint32_t m[16];
	uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];

	for (unsigned i = 0; i < 16u; i++)
		m[i] = (uint32_t)p[i * 4] |
		       ((uint32_t)p[i * 4 + 1] << 8) |
		       ((uint32_t)p[i * 4 + 2] << 16) |
		       ((uint32_t)p[i * 4 + 3] << 24);

	for (unsigned i = 0; i < 64u; i++) {
		uint32_t f;
		unsigned g;

		if (i < 16u) {
			f = (b & cc) | (~b & d);
			g = i;
		} else if (i < 32u) {
			f = (d & b) | (~d & cc);
			g = (5u * i + 1u) & 15u;
		} else if (i < 48u) {
			f = b ^ cc ^ d;
			g = (3u * i + 5u) & 15u;
		} else {
			f = cc ^ (b | ~d);
			g = (7u * i) & 15u;
		}

		f += a + md5_k[i] + m[g];
		a = d;
		d = cc;
		cc = b;
		b += rotl32(f, md5_s[i]);
	}

	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
}

void mcufw_md5_init(md5_ctx_t *c)
{
	c->h[0] = 0x67452301u;
	c->h[1] = 0xefcdab89u;
	c->h[2] = 0x98badcfeu;
	c->h[3] = 0x10325476u;
	c->bytes = 0;
	mcufw_zero(c->buf, sizeof(c->buf));
}

void mcufw_md5_update(md5_ctx_t *c, const uint8_t *data, size_t len)
{
	size_t fill = (size_t)(c->bytes & 63u);

	c->bytes += len;

	if (fill) {
		size_t take = 64u - fill;

		if (take > len)
			take = len;
		mcufw_copy(c->buf + fill, data, take);
		data += take;
		len -= take;
		if (fill + take < 64u)
			return;
		md5_block(c, c->buf);
	}

	while (len >= 64u) {
		md5_block(c, data);
		data += 64u;
		len -= 64u;
	}
	if (len)
		mcufw_copy(c->buf, data, len);
}

void mcufw_md5_final(md5_ctx_t *c, uint8_t out[MCUFW_MD5_LEN])
{
	uint64_t bits = c->bytes * 8u;
	uint8_t pad[72];
	size_t fill = (size_t)(c->bytes & 63u);
	size_t padlen = (fill < 56u) ? (56u - fill) : (120u - fill);

	mcufw_zero(pad, sizeof(pad));
	pad[0] = 0x80u;
	for (unsigned i = 0; i < 8u; i++)
		pad[padlen + i] = (uint8_t)(bits >> (8u * i));

	mcufw_md5_update(c, pad, padlen + 8u);

	for (unsigned i = 0; i < 4u; i++) {
		out[i * 4 + 0] = (uint8_t)(c->h[i]);
		out[i * 4 + 1] = (uint8_t)(c->h[i] >> 8);
		out[i * 4 + 2] = (uint8_t)(c->h[i] >> 16);
		out[i * 4 + 3] = (uint8_t)(c->h[i] >> 24);
	}
}

void mcufw_md5(const uint8_t *data, size_t len, uint8_t out[MCUFW_MD5_LEN])
{
	md5_ctx_t c;

	mcufw_md5_init(&c);
	mcufw_md5_update(&c, data, len);
	mcufw_md5_final(&c, out);
}

void mcufw_md5_2(const uint8_t *a, size_t alen,
		 const uint8_t *b, size_t blen, uint8_t out[MCUFW_MD5_LEN])
{
	md5_ctx_t c;

	mcufw_md5_init(&c);
	mcufw_md5_update(&c, a, alen);
	mcufw_md5_update(&c, b, blen);
	mcufw_md5_final(&c, out);
}

/* ------------------------------------------------------------------- AES --- */
static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static uint8_t aes_inv_sbox[256];
static int aes_inv_ready;

static void aes_inv_sbox_build(void)
{
	if (aes_inv_ready)
		return;
	for (unsigned i = 0; i < 256u; i++)
		aes_inv_sbox[aes_sbox[i]] = (uint8_t)i;
	aes_inv_ready = 1;
}

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ (((x >> 7) & 1u) ? 0x1bu : 0x00u));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	for (unsigned i = 0; i < 8u; i++) {
		if (b & 1u)
			p ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return p;
}

void mcufw_aes128_init(mcufw_aes128_t *ctx, const uint8_t key[MCUFW_AES_BLOCK])
{
	static const uint8_t rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10,
					  0x20, 0x40, 0x80, 0x1b, 0x36 };
	uint8_t w[176];
	unsigned i;

	for (i = 0; i < 16u; i++)
		w[i] = key[i];

	for (i = 16u; i < 176u; i += 4u) {
		uint8_t t[4] = { w[i - 4], w[i - 3], w[i - 2], w[i - 1] };

		if ((i & 15u) == 0u) {
			uint8_t t0 = t[0];

			t[0] = (uint8_t)(aes_sbox[t[1]] ^ rcon[i / 16u - 1u]);
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[t0];
		}
		for (unsigned j = 0; j < 4u; j++)
			w[i + j] = (uint8_t)(w[i - 16u + j] ^ t[j]);
	}

	for (i = 0; i < 44u; i++)
		ctx->rk[i] = (uint32_t)w[i * 4] |
			     ((uint32_t)w[i * 4 + 1] << 8) |
			     ((uint32_t)w[i * 4 + 2] << 16) |
			     ((uint32_t)w[i * 4 + 3] << 24);
}

static void aes_add_round_key(uint8_t s[16], const mcufw_aes128_t *ctx, unsigned round)
{
	for (unsigned c = 0; c < 4u; c++) {
		uint32_t k = ctx->rk[round * 4u + c];

		s[c * 4 + 0] ^= (uint8_t)(k);
		s[c * 4 + 1] ^= (uint8_t)(k >> 8);
		s[c * 4 + 2] ^= (uint8_t)(k >> 16);
		s[c * 4 + 3] ^= (uint8_t)(k >> 24);
	}
}

static void aes_sub_bytes(uint8_t s[16])
{
	for (unsigned i = 0; i < 16u; i++)
		s[i] = aes_sbox[s[i]];
}

static void aes_inv_sub_bytes(uint8_t s[16])
{
	aes_inv_sbox_build();
	for (unsigned i = 0; i < 16u; i++)
		s[i] = aes_inv_sbox[s[i]];
}

/* Column-major state: s[4*c + r] = row r, column c. */
static void aes_shift_rows(uint8_t s[16])
{
	uint8_t t[16];

	for (unsigned c = 0; c < 4u; c++)
		for (unsigned r = 0; r < 4u; r++)
			t[c * 4 + r] = s[((c + r) & 3u) * 4 + r];

	mcufw_copy(s, t, 16u);
}

static void aes_inv_shift_rows(uint8_t s[16])
{
	uint8_t t[16];

	for (unsigned c = 0; c < 4u; c++)
		for (unsigned r = 0; r < 4u; r++)
			t[((c + r) & 3u) * 4 + r] = s[c * 4 + r];

	mcufw_copy(s, t, 16u);
}

static void aes_mix_columns(uint8_t s[16])
{
	for (unsigned c = 0; c < 4u; c++) {
		uint8_t *p = &s[c * 4];
		uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];

		p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
		p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
		p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
		p[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
	}
}

static void aes_inv_mix_columns(uint8_t s[16])
{
	for (unsigned c = 0; c < 4u; c++) {
		uint8_t *p = &s[c * 4];
		uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];

		p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^
				 gmul(a2, 13) ^ gmul(a3, 9));
		p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^
				 gmul(a2, 11) ^ gmul(a3, 13));
		p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^
				 gmul(a2, 14) ^ gmul(a3, 11));
		p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^
				 gmul(a2, 9) ^ gmul(a3, 14));
	}
}

void mcufw_aes128_encrypt_block(const mcufw_aes128_t *ctx,
				const uint8_t in[MCUFW_AES_BLOCK],
				uint8_t out[MCUFW_AES_BLOCK])
{
	uint8_t s[16];

	mcufw_copy(s, in, 16u);
	aes_add_round_key(s, ctx, 0);
	for (unsigned round = 1; round <= 10u; round++) {
		aes_sub_bytes(s);
		aes_shift_rows(s);
		if (round != 10u)
			aes_mix_columns(s);
		aes_add_round_key(s, ctx, round);
	}
	mcufw_copy(out, s, 16u);
}

void mcufw_aes128_decrypt_block(const mcufw_aes128_t *ctx,
				const uint8_t in[MCUFW_AES_BLOCK],
				uint8_t out[MCUFW_AES_BLOCK])
{
	uint8_t s[16];

	mcufw_copy(s, in, 16u);
	aes_add_round_key(s, ctx, 10);
	for (unsigned round = 10; round-- > 0u;) {
		aes_inv_shift_rows(s);
		aes_inv_sub_bytes(s);
		aes_add_round_key(s, ctx, round);
		if (round != 0u)
			aes_inv_mix_columns(s);
	}
	mcufw_copy(out, s, 16u);
}

size_t mcufw_aes128_cbc_encrypt(const uint8_t key[MCUFW_AES_BLOCK],
				const uint8_t iv[MCUFW_AES_BLOCK],
				const uint8_t *in, size_t len, uint8_t *out)
{
	mcufw_aes128_t ctx;
	uint8_t chain[16];
	uint8_t pad = (uint8_t)(16u - (len & 15u));
	size_t padded = len + pad;
	size_t i;

	mcufw_aes128_init(&ctx, key);
	mcufw_copy(chain, iv, 16u);

	/* Work in place-safe fashion: encrypt blocks of the padded input.  The
	 * padding bytes are appended logically (index >= len -> pad value). */
	for (i = 0; i < padded; i += 16u) {
		uint8_t block[16];

		for (unsigned j = 0; j < 16u; j++) {
			size_t idx = i + j;

			block[j] = (uint8_t)((idx < len ? in[idx] : pad) ^ chain[j]);
		}
		mcufw_aes128_encrypt_block(&ctx, block, chain);
		mcufw_copy(out + i, chain, 16u);
	}
	return padded;
}

size_t mcufw_aes128_cbc_decrypt(const uint8_t key[MCUFW_AES_BLOCK],
				const uint8_t iv[MCUFW_AES_BLOCK],
				const uint8_t *in, size_t len, uint8_t *out)
{
	mcufw_aes128_t ctx;
	uint8_t chain[16];
	uint8_t prev[16];
	size_t i;

	if (len == 0 || (len & 15u))
		return 0;

	mcufw_aes128_init(&ctx, key);
	mcufw_copy(chain, iv, 16u);

	for (i = 0; i < len; i += 16u) {
		uint8_t plain[16];

		mcufw_copy(prev, in + i, 16u);
		mcufw_aes128_decrypt_block(&ctx, in + i, plain);
		for (unsigned j = 0; j < 16u; j++)
			out[i + j] = (uint8_t)(plain[j] ^ chain[j]);
		mcufw_copy(chain, prev, 16u);
	}

	/* PKCS#7 unpad: the last byte must be 1..16 and repeated that often. */
	{
		uint8_t pad = out[len - 1];

		if (pad == 0u || pad > 16u)
			return 0;
		for (unsigned j = 0; j < pad; j++)
			if (out[len - 1 - j] != pad)
				return 0;
		return len - pad;
	}
}
