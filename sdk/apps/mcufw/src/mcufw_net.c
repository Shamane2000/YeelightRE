/*
 * mcufw_net.c -- pump functions for the two lamp front-ends (phase 3.8).
 *
 * Translation-unit local, no sockets, no registers, no RTOS: the transport is
 * a struct of function pointers (mcufw_net.h).  On the target this is the raw
 * lwIP PCB layer (mcufw_net_lwip.c), in the host test an injectable FIFO
 * (mcufw_net_host.c) -- so the very same dispatch code that runs on the lamp
 * is asserted with real request bytes on the build host.
 */
#include "mcufw_net.h"
#include "mcufw_lan_rpc.h"

static const mcufw_net_transport_t *g_tr;
static mcufw_net_stats_t g_net;
static mcufw_miio_ctx_t g_miio;

static uint8_t g_udp_rx[MCUFW_NET_UDP_PKT];
static uint8_t g_udp_tx[MCUFW_NET_UDP_PKT];
static char g_tcp_rx[MCUFW_NET_LINE_MAX];
static char g_tcp_tx[MCUFW_NET_LINE_MAX];
static mcufw_rpc_request_t g_req;

void mcufw_net_bind_transport(const mcufw_net_transport_t *t)
{
	g_tr = t;
}

int mcufw_net_init(const mcufw_net_transport_t *t, uint32_t did)
{
	g_tr = t;
	mcufw_net_stats_reset();
	mcufw_miio_ctx_init(&g_miio, did);
	/* A transport that cannot bind its ports is reported upwards; the loop
	 * keeps ticking either way (a dead channel must not stop the lamp). */
	if (g_tr && (g_tr->udp_recv == 0 || g_tr->tcp_recv == 0))
		return -1;
	return 0;
}

void mcufw_net_stats_reset(void)
{
	g_net.udp_pumps = 0u;
	g_net.tcp_pumps = 0u;
	g_net.udp_datagrams = 0u;
	g_net.tcp_requests = 0u;
	g_net.udp_replies = 0u;
	g_net.tcp_replies = 0u;
	g_net.udp_dropped = 0u;
	g_net.rpc_errors = 0u;
	g_net.udp_bytes = 0u;
	g_net.tcp_bytes = 0u;
}

const mcufw_net_stats_t *mcufw_net_stats(void)
{
	return &g_net;
}

mcufw_miio_ctx_t *mcufw_net_miio(void)
{
	return &g_miio;
}

unsigned mcufw_net_pump_udp(void)
{
	unsigned sent = 0u;
	unsigned k;

	g_net.udp_pumps++;

	if (!g_tr || !g_tr->udp_recv)
		return 0u;

	for (k = 0u; k < MCUFW_NET_UDP_BURST; k++) {
		size_t len = 0u;
		size_t out_len = 0u;
		int rc;
		int mrc;

		rc = g_tr->udp_recv(g_udp_rx, sizeof(g_udp_rx), &len);
		if (rc <= 0)
			break;

		g_net.udp_datagrams++;
		g_net.udp_bytes += (uint32_t)len;

		mrc = mcufw_miio_handle(&g_miio, g_udp_rx, len,
					g_udp_tx, sizeof(g_udp_tx), &out_len);
		if (mrc != MCUFW_MIIO_OK || out_len == 0u) {
			g_net.udp_dropped++;
			continue;
		}
		if (g_tr->udp_send)
			g_tr->udp_send(g_udp_tx, out_len);
		g_net.udp_replies++;
		sent++;
	}

	return sent;
}

unsigned mcufw_net_pump_tcp(void)
{
	unsigned sent = 0u;
	unsigned k;

	g_net.tcp_pumps++;

	if (!g_tr || !g_tr->tcp_recv)
		return 0u;

	for (k = 0u; k < MCUFW_NET_TCP_BURST; k++) {
		size_t len = 0u;
		int rc;
		int prc;
		int wlen;

		rc = g_tr->tcp_recv(g_tcp_rx, sizeof(g_tcp_rx), &len);
		if (rc <= 0)
			break;

		g_net.tcp_requests++;
		g_net.tcp_bytes += (uint32_t)len;

		prc = mcufw_rpc_parse(g_tcp_rx, len, &g_req);
		if (prc != MCUFW_RPC_OK) {
			g_net.rpc_errors++;
			continue;
		}
		wlen = mcufw_rpc_dispatch(&g_req, g_tcp_tx, sizeof(g_tcp_tx));
		if (wlen <= 0) {
			g_net.rpc_errors++;
			continue;
		}
		if (g_tr->tcp_send)
			g_tr->tcp_send(g_tcp_tx, (size_t)wlen);
		g_net.tcp_replies++;
		sent++;
	}

	return sent;
}
