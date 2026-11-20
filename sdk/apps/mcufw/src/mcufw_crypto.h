/*
 * mcufw_crypto.h -- freestanding MD5 + AES-128-CBC for the miIO channel.
 *
 * Why own code instead of the vendor crypto:
 *   libwmsdk.a ships md5_fp0.o (wolfSSL wc_Md5*) and aes.o (mbedtls-style
 *   aes_decrypt_init/aes_decrypt).  Both would link, but their struct layouts
 *   (Md5, aes_decrypt_ctx) are not part of the include tree we have
 *   (sdk/src/incl carries only platform/os/freertos + the mw300 register
 *   headers), so declaring the context structs would be guesswork.  A wrong
 *   sizeof() in a crypto context is a silent memory corruption, not a link
 *   error -- so the miIO layer uses these small, self-contained, host-testable
 *   implementations instead (FIPS-197 / RFC 1321 known-answer vectors run in
 *   mcufw_selftest.c on the target *and* in the host test).
 *
 * miIO wire crypto (proven in docs/06 §3, colour firmware):
 *   key = MD5(token)                      16 B
 *   iv  = MD5(key || token)               16 B
 *   payload = AES-128-CBC(key, iv, JSON)  with PKCS#7 padding
 */
#ifndef MCUFW_CRYPTO_H
#define MCUFW_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_MD5_LEN     16u
#define MCUFW_AES_BLOCK   16u

typedef struct {
	uint32_t h[4];
	uint64_t bytes;
	uint8_t  buf[64];
} mcufw_md5_ctx_t;

void mcufw_md5_init(mcufw_md5_ctx_t *c);
void mcufw_md5_update(mcufw_md5_ctx_t *c, const uint8_t *data, size_t len);
void mcufw_md5_final(mcufw_md5_ctx_t *c, uint8_t out[MCUFW_MD5_LEN]);

/* MD5 over one contiguous buffer (hello checksum + key derivation). */
void mcufw_md5(const uint8_t *data, size_t len, uint8_t out[MCUFW_MD5_LEN]);

/* MD5 over two buffers without a temp copy: md5(a || b).  Used for
 * key = MD5(key||token) and checksum = MD5(header16 || token). */
void mcufw_md5_2(const uint8_t *a, size_t alen,
		 const uint8_t *b, size_t blen, uint8_t out[MCUFW_MD5_LEN]);

typedef struct {
	uint32_t rk[44];        /* 11 round keys x 4 words */
} mcufw_aes128_t;

void mcufw_aes128_init(mcufw_aes128_t *ctx, const uint8_t key[MCUFW_AES_BLOCK]);
void mcufw_aes128_encrypt_block(const mcufw_aes128_t *ctx,
				const uint8_t in[MCUFW_AES_BLOCK],
				uint8_t out[MCUFW_AES_BLOCK]);
void mcufw_aes128_decrypt_block(const mcufw_aes128_t *ctx,
				const uint8_t in[MCUFW_AES_BLOCK],
				uint8_t out[MCUFW_AES_BLOCK]);

/* CBC with PKCS#7.  `out` must have room for len + 16 bytes (in == out is
 * allowed).  Returns the padded/plain length, or 0 on a padding error
 * (decrypt only). */
size_t mcufw_aes128_cbc_encrypt(const uint8_t key[MCUFW_AES_BLOCK],
				const uint8_t iv[MCUFW_AES_BLOCK],
				const uint8_t *in, size_t len, uint8_t *out);
size_t mcufw_aes128_cbc_decrypt(const uint8_t key[MCUFW_AES_BLOCK],
				const uint8_t iv[MCUFW_AES_BLOCK],
				const uint8_t *in, size_t len, uint8_t *out);

#endif /* MCUFW_CRYPTO_H */
