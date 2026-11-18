/*
 * mcufw_net_host.c -- FIFO transport + call counters for the host test.
 *
 * Implements exactly the mcufw_net_transport_t contract of mcufw_net.h:
 * reads never block, they return 0 when the queue is empty.
 */
#include "mcufw_net_host.h"

#include <string.h>

static uint8_t g_udp_in[MCUFW_NET_HOST_SLOTS][MCUFW_MIIO_MAX_PKT];
static size_t g_udp_in_len[MCUFW_NET_HOST_SLOTS];
static unsigned g_udp_rd;
static unsigned g_udp_wr;

static char g_line_in[MCUFW_NET_HOST_SLOTS][MCUFW_NET_LINE_MAX];
static size_t g_line_in_len[MCUFW_NET_HOST_SLOTS];
static unsigned g_line_rd;
static unsigned g_line_wr;

static uint8_t g_udp_tx[MCUFW_MIIO_MAX_PKT];
static size_t g_udp_tx_len;
static char g_tcp_tx[MCUFW_NET_LINE_MAX];

static mcufw_net_host_stats_t g_host;

void mcufw_net_host_reset(void)
{
	memset(g_udp_in_len, 0, sizeof(g_udp_in_len));
	memset(g_line_in_len, 0, sizeof(g_line_in_len));
	g_udp_rd = 0u;
	g_udp_wr = 0u;
	g_line_rd = 0u;
	g_line_wr = 0u;
	g_udp_tx_len = 0u;
	g_tcp_tx[0] = '\0';
	memset(&g_host, 0, sizeof(g_host));
}

int mcufw_net_host_push_udp(const uint8_t *data, size_t len)
{
	unsigned next;

	if (!data || len == 0u || len > MCUFW_MIIO_MAX_PKT)
		return -1;
	next = (g_udp_wr + 1u) % MCUFW_NET_HOST_SLOTS;
	if (next == g_udp_rd)
		return -1;
	memcpy(g_udp_in[g_udp_wr], data, len);
	g_udp_in_len[g_udp_wr] = len;
	g_udp_wr = next;
	g_host.udp_datagrams_in++;
	return 0;
}

int mcufw_net_host_push_line(const char *line, size_t len)
{
	unsigned next;

	if (!line || len == 0u || len >= MCUFW_NET_LINE_MAX)
		return -1;
	next = (g_line_wr + 1u) % MCUFW_NET_HOST_SLOTS;
	if (next == g_line_rd)
		return -1;
	memcpy(g_line_in[g_line_wr], line, len);
	g_line_in[g_line_wr][len] = '\0';
	g_line_in_len[g_line_wr] = len;
	g_line_wr = next;
	g_host.tcp_lines_in++;
	return 0;
}

static int host_udp_recv(uint8_t *buf, size_t cap, size_t *len)
{
	g_host.udp_recv_calls++;
	if (g_udp_rd == g_udp_wr)
		return 0;
	if (g_udp_in_len[g_udp_rd] > cap)
		return -1;
	memcpy(buf, g_udp_in[g_udp_rd], g_udp_in_len[g_udp_rd]);
	*len = g_udp_in_len[g_udp_rd];
	g_udp_rd = (g_udp_rd + 1u) % MCUFW_NET_HOST_SLOTS;
	return 1;
}

static void host_udp_send(const uint8_t *buf, size_t len)
{
	g_host.udp_sends++;
	g_host.udp_send_bytes = (uint32_t)len;
	if (len > sizeof(g_udp_tx))
		len = sizeof(g_udp_tx);
	memcpy(g_udp_tx, buf, len);
	g_udp_tx_len = len;
}

static int host_tcp_recv(char *buf, size_t cap, size_t *len)
{
	g_host.tcp_recv_calls++;
	if (g_line_rd == g_line_wr)
		return 0;
	if (g_line_in_len[g_line_rd] + 1u > cap)
		return -1;
	memcpy(buf, g_line_in[g_line_rd], g_line_in_len[g_line_rd] + 1u);
	*len = g_line_in_len[g_line_rd];
	g_line_rd = (g_line_rd + 1u) % MCUFW_NET_HOST_SLOTS;
	return 1;
}

static void host_tcp_send(const char *buf, size_t len)
{
	g_host.tcp_sends++;
	g_host.tcp_send_bytes = (uint32_t)len;
	if (len >= sizeof(g_tcp_tx))
		len = sizeof(g_tcp_tx) - 1u;
	memcpy(g_tcp_tx, buf, len);
	g_tcp_tx[len] = '\0';
}

static const mcufw_net_transport_t g_host_tr = {
	host_udp_recv,
	host_udp_send,
	host_tcp_recv,
	host_tcp_send
};

const mcufw_net_transport_t *mcufw_net_host_transport(void)
{
	return &g_host_tr;
}

const mcufw_net_host_stats_t *mcufw_net_host_stats(void)
{
	return &g_host;
}

size_t mcufw_net_host_last_udp_tx(uint8_t *dst, size_t cap)
{
	size_t n = g_udp_tx_len;

	if (!dst)
		return n;
	if (n > cap)
		n = cap;
	memcpy(dst, g_udp_tx, n);
	return n;
}

const char *mcufw_net_host_last_tcp_tx(void)
{
	return g_tcp_tx;
}
