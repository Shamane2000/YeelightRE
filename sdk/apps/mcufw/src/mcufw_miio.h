/*
 * mcufw_miio.h -- miIO-compatible UDP channel (54321) for the mcufw.
 *
 * Scope of phase 3.4 (skeleton, no LAN-JSON-RPC, no MQTT):
 *   - 32-byte miIO packet header (magic 0x2131, length, did, stamp, MD5)
 *   - `hello` handshake incl. token hand-out and the "unprovisioned" detection
 *   - MD5 header checksum + AES-128-CBC payload (key = MD5(token),
 *     iv = MD5(key||token)) -- the exact wire crypto proven in docs/06 §3
 *   - method extraction + dispatch table; `miIO.ota` is the only OTA channel
 *     (docs/06 §0: LAN 55443 is double-locked against OTA, miIO is not)
 *
 * Everything in mcufw_miio.c is a pure function over byte buffers: no socket,
 * no RTOS, no globals.  That is what makes it testable in the host self-test
 * (sdk/apps/mcufw/host_test.c) and identical on the target.
 *
 * The UDP transport (lwip_socket/lwip_bind/lwip_recvfrom) is *not* part of this
 * translation unit: lwIP core is not linked in 3.4 (see docs/10 §4), so the
 * socket loop lands with the RTOS/lwIP bring-up and only calls
 * mcufw_miio_handle() per datagram.
 */
#ifndef MCUFW_MIIO_H
#define MCUFW_MIIO_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_MIIO_PORT        54321u
#define MCUFW_MIIO_HDR_LEN     32u
#define MCUFW_MIIO_MAGIC       0x2131u
#define MCUFW_MIIO_MAX_PKT     1472u
#define MCUFW_MIIO_TOKEN_LEN   16u
#define MCUFW_MIIO_METHOD_MAX  23u

/* Return codes of mcufw_miio_handle().  0 = reply produced (or silently
 * dropped); everything else names the rejected stage. */
enum {
	MCUFW_MIIO_OK = 0,
	MCUFW_MIIO_E_SHORT = 1,       /* < 32 B header                        */
	MCUFW_MIIO_E_MAGIC = 2,       /* header[0..2] != 0x2131                */
	MCUFW_MIIO_E_LEN = 3,         /* length field != datagram length      */
	MCUFW_MIIO_E_CHECKSUM = 4,    /* MD5(header||token) mismatch          */
	MCUFW_MIIO_E_TOKEN_MISSING = 5, /* encrypted request, token is ff..ff  */
	MCUFW_MIIO_E_CRYPTO = 6,      /* AES-CBC padding/decrypt failed       */
	MCUFW_MIIO_E_JSON = 7,        /* no "method" field                    */
	MCUFW_MIIO_E_UNSUPPORTED = 8, /* method not in the miIO table         */
	MCUFW_MIIO_E_ROOM = 9,        /* caller buffer too small              */
};

typedef struct {
	uint32_t did;                    /* device id (live .115: 0x05eb8034)  */
	uint32_t stamp;                  /* last accepted stamp (replay guard)  */
	uint8_t  token[MCUFW_MIIO_TOKEN_LEN];
	uint8_t  have_token;             /* 0 => token is ff..ff (not paired)   */
	uint32_t ota_requests;           /* miIO.ota calls seen                 */
	uint32_t rejected;               /* packets dropped in a guard stage    */
	char     last_method[MCUFW_MIIO_METHOD_MAX + 1u];
} mcufw_miio_ctx_t;

void mcufw_miio_ctx_init(mcufw_miio_ctx_t *ctx, uint32_t did);

/* Hand a 16-byte token to the context.  An all-0xff token means "not
 * provisioned" and is what the reference lamp returns today (docs/06 §0). */
int mcufw_miio_set_token(mcufw_miio_ctx_t *ctx,
			 const uint8_t token[MCUFW_MIIO_TOKEN_LEN]);

/* Process one datagram.  Writes the reply (if any) into out/out_len; a
 * return value of MCUFW_MIIO_E_* means "dropped", and *out_len is 0. */
int mcufw_miio_handle(mcufw_miio_ctx_t *ctx,
		      const uint8_t *in, size_t in_len,
		      uint8_t *out, size_t out_cap, size_t *out_len);

const char *mcufw_miio_strerror(int rc);

/* Self-test of the whole channel (known-answer crypto vectors + hello +
 * encrypted miIO.ota round trip).  Returns 0 when everything passed, else a
 * bitmask of failed cases. */
uint32_t mcufw_miio_selftest(void);

#endif /* MCUFW_MIIO_H */
