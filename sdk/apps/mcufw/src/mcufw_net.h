/*
 * mcufw_net.h -- non-blocking network service seam (phase 3.8).
 *
 * Both lamp front-ends are pure functions over byte buffers:
 *
 *   mcufw_miio_handle()   -- miIO datagram on UDP 54321
 *   mcufw_rpc_parse()/mcufw_rpc_dispatch() -- LAN JSON-RPC line on TCP 55443
 *
 * Until 3.7 nothing ever called them from a loop; this module owns the two
 * pump functions that the runtime service loop calls once per tick.  The
 * actual transport (raw lwIP PCB on the target, injectable FIFO in the host
 * test) is plugged in through mcufw_net_transport_t, so the pumps themselves
 * stay free of sockets, registers and RTOS primitives -- and are therefore
 * asserted byte-exactly by the host test.
 *
 * Contract of one tick (see mcufw_net_pump_udp / _tcp):
 *   - never blocks: at most MCUFW_NET_*_BURST packets, then return
 *   - returns the number of packets that produced a sent reply
 *   - counts every call in mcufw_net_stats() so the loop can prove that the
 *     channels are really serviced per tick (not just "linked in").
 */
#ifndef MCUFW_NET_H
#define MCUFW_NET_H

#include <stddef.h>
#include <stdint.h>

#include "mcufw_miio.h"

/* Bounded work per tick: a flooded socket must not starve the lamp. */
#define MCUFW_NET_UDP_BURST 4u
#define MCUFW_NET_TCP_BURST 2u
/* Longest accepted LAN-RPC line (the spec's requests are well below 512). */
#define MCUFW_NET_LINE_MAX  256u
/* Longest accepted miIO datagram.  mcufw_miio.h allows 1472, but every real
 * miIO request/response is at most a few hundred bytes (32 B header + JSON
 * command); 512 keeps the SRAM1 budget of the image (two pump buffers here
 * plus two transport slots in mcufw_net_lwip.c) inside the linker window. */
#define MCUFW_NET_UDP_PKT   512u

typedef struct mcufw_net_transport {
	/* Non-blocking read of one pending datagram.
	 * returns 1 = datagram in buf, 0 = nothing pending, < 0 = transport error */
	int  (*udp_recv)(uint8_t *buf, size_t cap, size_t *len);
	/* Send one reply datagram (peer taken from the last udp_recv). */
	void (*udp_send)(const uint8_t *buf, size_t len);
	/* Non-blocking read of one pending request line (NUL-terminated, len
	 * without the terminator).  Same return convention as udp_recv. */
	int  (*tcp_recv)(char *buf, size_t cap, size_t *len);
	/* Send one reply line. */
	void (*tcp_send)(const char *buf, size_t len);
} mcufw_net_transport_t;

typedef struct {
	uint32_t udp_pumps;     /* mcufw_net_pump_udp() calls  */
	uint32_t tcp_pumps;     /* mcufw_net_pump_tcp() calls  */
	uint32_t udp_datagrams; /* datagrams read from the transport */
	uint32_t tcp_requests;  /* request lines read               */
	uint32_t udp_replies;   /* datagrams answered               */
	uint32_t tcp_replies;   /* lines answered                   */
	uint32_t udp_dropped;   /* datagrams rejected by mcufw_miio_handle */
	uint32_t rpc_errors;    /* parse / dispatch failures               */
	uint32_t udp_bytes;     /* payload bytes received via UDP          */
	uint32_t tcp_bytes;     /* payload bytes received via TCP          */
} mcufw_net_stats_t;

/* Plug in a transport (NULL = no transport: pumps still count, move nothing)
 * and bring the miIO context up for the given device id.  Returns 0 on
 * success, -1 if an lwIP-style transport reported a bind failure. */
int mcufw_net_init(const mcufw_net_transport_t *t, uint32_t did);

/* Overwrite the transport without touching the miIO context (host test). */
void mcufw_net_bind_transport(const mcufw_net_transport_t *t);

/* Statistics only; the transport stays plugged in. */
void mcufw_net_stats_reset(void);

const mcufw_net_stats_t *mcufw_net_stats(void);

/* The single miIO context both the datagram pump and the diagnostics use. */
mcufw_miio_ctx_t *mcufw_net_miio(void);

/* One non-blocking service pass over UDP 54321 / TCP 55443. */
unsigned mcufw_net_pump_udp(void);
unsigned mcufw_net_pump_tcp(void);

#endif /* MCUFW_NET_H */
