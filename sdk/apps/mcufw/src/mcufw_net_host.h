/*
 * mcufw_net_host.h -- injectable transport for the host test (phase 3.8).
 *
 * The host build has no lwIP: this FIFO transport stands in for the raw PCB
 * layer and records everything the pumps hand back, so a test can assert real
 * request bytes -> real dispatch -> real reply bytes without any socket.
 */
#ifndef MCUFW_NET_HOST_H
#define MCUFW_NET_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "mcufw_net.h"

#define MCUFW_NET_HOST_SLOTS 6u

typedef struct {
	uint32_t udp_recv_calls;   /* udp_recv() invocations               */
	uint32_t udp_datagrams_in; /* datagrams queued by the test         */
	uint32_t udp_sends;        /* reply datagrams handed to the send fn*/
	uint32_t udp_send_bytes;   /* bytes of the last reply datagram     */
	uint32_t tcp_recv_calls;
	uint32_t tcp_lines_in;
	uint32_t tcp_sends;
	uint32_t tcp_send_bytes;
} mcufw_net_host_stats_t;

void mcufw_net_host_reset(void);

/* Queue one request for the next pump pass.  Returns 0, -1 when full. */
int mcufw_net_host_push_udp(const uint8_t *data, size_t len);
int mcufw_net_host_push_line(const char *line, size_t len);

const mcufw_net_host_stats_t *mcufw_net_host_stats(void);
const mcufw_net_transport_t *mcufw_net_host_transport(void);

/* Copy of the last reply datagram / last reply line (NUL-terminated). */
size_t mcufw_net_host_last_udp_tx(uint8_t *dst, size_t cap);
const char *mcufw_net_host_last_tcp_tx(void);

#endif /* MCUFW_NET_HOST_H */
