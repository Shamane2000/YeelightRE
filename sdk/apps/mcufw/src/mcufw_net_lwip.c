/*
 * mcufw_net_lwip.c -- raw lwIP PCB transport (phase 3.8, target only).
 *
 *   UDP 54321 : one udp_pcb, receive callback copies the datagram into a
 *               2-slot FIFO.  udp_send answers the peer it came from.
 *   TCP 55443 : one listening tcp_pcb, exactly one client connection (the
 *               Yeelight LAN-RPC front-end is single-session by design),
 *               receive callback splits the stream into '\n'-terminated lines
 *               and queues them.
 *
 * Nothing in the callbacks touches the lamp state: they only move bytes into
 * the FIFOs that mcufw_net_pump_*() drains from the service loop.  That is
 * what keeps mcufw_pwm the single writer on GPT0 CMR0 -- no register write can
 * ever happen from an interrupt / tcpip-thread context.
 *
 * The netif is registered but has no IP yet (AP join = phase 5).  Binding to
 * 0.0.0.0 works without a link, so the ports are owned from boot on and the
 * first datagram after an AP join is served without any further code change.
 */
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include "mcufw_lan_rpc.h"
#include "mcufw_net_lwip.h"
#include "mcufw_http_portal.h"

/* Provided by mcufw_lwip.c (no public header yet). */
int mcufw_lwip_ready(void);
void mcufw_lwip_init(void);

#define RX_SLOTS 2u

typedef struct {
	uint8_t  data[MCUFW_NET_UDP_PKT];
	size_t   len;
	ip_addr_t src;
	uint16_t sport;
} udp_slot_t;

static udp_slot_t g_udp_q[RX_SLOTS];
static unsigned g_udp_rd;
static unsigned g_udp_wr;
static ip_addr_t g_udp_peer;
static uint16_t g_udp_peer_port;

static char g_line_q[RX_SLOTS][MCUFW_NET_LINE_MAX];
static size_t g_line_q_len[RX_SLOTS];
static unsigned g_line_rd;
static unsigned g_line_wr;
static char g_line_cur[MCUFW_NET_LINE_MAX];
static size_t g_line_cur_len;

static struct udp_pcb *g_udp_pcb;
static struct tcp_pcb *g_listen_pcb;
static struct tcp_pcb *g_conn_pcb;
static ip_addr_t g_any_addr;
static int g_bound;

/* ------------------------------------------------------------ UDP 54321 -- */
static void udp_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
		      ip_addr_t *addr, u16_t port)
{
	unsigned next;

	(void)arg;
	(void)pcb;

	if (!p)
		return;

	next = (g_udp_wr + 1u) % RX_SLOTS;
	if (next != g_udp_rd && p->tot_len <= (u16_t)sizeof(g_udp_q[g_udp_wr].data)) {
		udp_slot_t *s = &g_udp_q[g_udp_wr];

		s->len = (size_t)pbuf_copy_partial(p, s->data,
						   (u16_t)sizeof(s->data), 0);
		if (addr)
			ip_addr_copy(s->src, *addr);
		s->sport = port;
		g_udp_wr = next;
	}
	pbuf_free(p);
}

static int lwip_udp_recv(uint8_t *buf, size_t cap, size_t *len)
{
	udp_slot_t *s;

	if (g_udp_rd == g_udp_wr)
		return 0;
	s = &g_udp_q[g_udp_rd];
	if (s->len > cap)
		return -1;

	{
		size_t i;

		for (i = 0u; i < s->len; i++)
			buf[i] = s->data[i];
	}
	*len = s->len;
	ip_addr_copy(g_udp_peer, s->src);
	g_udp_peer_port = s->sport;
	g_udp_rd = (g_udp_rd + 1u) % RX_SLOTS;
	return 1;
}

static void lwip_udp_send(const uint8_t *buf, size_t len)
{
	struct pbuf *p;

	if (!g_udp_pcb || len == 0u)
		return;
	p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
	if (!p)
		return;
	pbuf_take(p, buf, (u16_t)len);
	(void)udp_sendto(g_udp_pcb, p, &g_udp_peer, g_udp_peer_port);
	pbuf_free(p);
}

/* ------------------------------------------------------------ TCP 55443 -- */
static void line_push(const char *line, size_t len)
{
	unsigned next = (g_line_wr + 1u) % RX_SLOTS;
	size_t i;

	if (next == g_line_rd || len >= MCUFW_NET_LINE_MAX)
		return;
	for (i = 0u; i < len; i++)
		g_line_q[g_line_wr][i] = line[i];
	g_line_q[g_line_wr][len] = '\0';
	g_line_q_len[g_line_wr] = len;
	g_line_wr = next;
}

/* Split one received chunk into '\n'-terminated request lines. */
static void line_feed(const void *data, size_t len)
{
	const char *p = (const char *)data;
	size_t i;

	for (i = 0u; i < len; i++) {
		char c = p[i];

		if (c == '\r')
			continue;
		if (c == '\n') {
			if (g_line_cur_len > 0u)
				line_push(g_line_cur, g_line_cur_len);
			g_line_cur_len = 0u;
			continue;
		}
		if (g_line_cur_len + 1u < sizeof(g_line_cur))
			g_line_cur[g_line_cur_len++] = c;
		else
			g_line_cur_len = 0u; /* oversized line: drop it */
	}
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
			 err_t err)
{
	(void)arg;

	if (!p) {
		/* Remote closed: tear the session down, the listen pcb stays. */
		tcp_recv(pcb, NULL);
		tcp_arg(pcb, NULL);
		if (pcb == g_conn_pcb)
			g_conn_pcb = NULL;
		if (err == ERR_OK)
			(void)tcp_close(pcb);
		return ERR_OK;
	}

	if (p->tot_len > 0u) {
		struct pbuf *q;

		/* lwIP 1.4.1 has no pbuf_skip(): walk the chain ourselves. */
		for (q = p; q != NULL && g_line_cur_len < sizeof(g_line_cur); q = q->next)
			line_feed(q->payload, (size_t)q->len);
		tcp_recved(pcb, p->tot_len);
	}
	pbuf_free(p);
	return ERR_OK;
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	(void)arg;

	if (err != ERR_OK || !newpcb)
		return ERR_VAL;
	if (g_conn_pcb) {
		/* Single session: refuse a second client. */
		tcp_abort(newpcb);
		return ERR_ABRT;
	}
	g_conn_pcb = newpcb;
	g_line_cur_len = 0u;
	tcp_arg(newpcb, NULL);
	tcp_recv(newpcb, tcp_recv_cb);
	tcp_nagle_disable(newpcb);
	tcp_accepted(g_listen_pcb);
	return ERR_OK;
}

static int lwip_tcp_recv(char *buf, size_t cap, size_t *len)
{
	size_t n;
	size_t i;

	if (g_line_rd == g_line_wr)
		return 0;
	n = g_line_q_len[g_line_rd];
	if (n + 1u > cap)
		return -1;
	for (i = 0u; i <= n; i++)
		buf[i] = g_line_q[g_line_rd][i];
	*len = n;
	g_line_rd = (g_line_rd + 1u) % RX_SLOTS;
	return 1;
}

static void lwip_tcp_send(const char *buf, size_t len)
{
	if (!g_conn_pcb || len == 0u)
		return;
	if (tcp_sndbuf(g_conn_pcb) < (u16_t)len)
		return;
	if (tcp_write(g_conn_pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK)
		return;
	(void)tcp_output(g_conn_pcb);
}


/* ----------------------------------------------------------- HTTP 8080 --- */
static struct tcp_pcb *g_http_listen;
static struct tcp_pcb *g_http_conn;
static char   g_http_rx[1024];
static size_t g_http_rx_len;
static int    g_http_ready;

static int find_hdr_end_and_clen(const char *b, size_t len, size_t *hdr_end, size_t *clen)
{
	*hdr_end = 0u;
	*clen = 0u;
	for (size_t i = 0u; i + 3u < len; i++) {
		if (b[i] == '\r' && b[i + 1u] == '\n' &&
		    b[i + 2u] == '\r' && b[i + 3u] == '\n') {
			*hdr_end = i + 4u;
			break;
		}
	}
	if (*hdr_end == 0u) return 0;
	for (size_t i = 0u; i + 16u < *hdr_end; i++) {
		static const char k[] = "Content-Length:";
		size_t m = 0u;
		while (k[m]) {
			char a = b[i + m];
			char c = k[m];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
			if (a != c) break;
			m++;
		}
		if (k[m] == '\0') {
			size_t p = i + m, v = 0u;
			while (p < *hdr_end && (b[p] == ' ' || b[p] == '\t')) p++;
			while (p < *hdr_end && b[p] >= '0' && b[p] <= '9') {
				v = v * 10u + (size_t)(b[p] - '0');
				p++;
			}
			*clen = v;
			break;
		}
	}
	return (len >= *hdr_end + *clen) ? 1 : 0;
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	(void)arg; (void)len;
	if (pcb == g_http_conn) {
		tcp_recv(pcb, NULL);
		tcp_sent(pcb, NULL);
		tcp_arg(pcb, NULL);
		g_http_conn = NULL;
		(void)tcp_close(pcb);
	}
	return ERR_OK;
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)arg;
	if (!p) {
		tcp_recv(pcb, NULL);
		tcp_sent(pcb, NULL);
		tcp_arg(pcb, NULL);
		if (pcb == g_http_conn) g_http_conn = NULL;
		if (err == ERR_OK) (void)tcp_close(pcb);
		return ERR_OK;
	}
	if (!g_http_ready && p->tot_len > 0u) {
		for (struct pbuf *q = p; q != NULL; q = q->next) {
			const char *d = (const char *)q->payload;
			for (u16_t i = 0u; i < q->len && g_http_rx_len + 1u < sizeof(g_http_rx); i++)
				g_http_rx[g_http_rx_len++] = d[i];
		}
		g_http_rx[g_http_rx_len] = '\0';
		size_t hd = 0u, cl = 0u;
		if (find_hdr_end_and_clen(g_http_rx, g_http_rx_len, &hd, &cl))
			g_http_ready = 1;
	}
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	(void)arg;
	if (err != ERR_OK || !newpcb) return ERR_VAL;
	if (g_http_conn) {
		tcp_abort(newpcb);
		return ERR_ABRT;
	}
	g_http_conn = newpcb;
	g_http_rx_len = 0u;
	g_http_ready = 0;
	tcp_arg(newpcb, NULL);
	tcp_recv(newpcb, http_recv_cb);
	tcp_sent(newpcb, http_sent_cb);
	tcp_accepted(g_http_listen);
	return ERR_OK;
}

int mcufw_net_lwip_http_rx(char *buf, size_t cap, size_t *len)
{
	if (!g_http_ready || !buf || !len || g_http_rx_len + 1u > cap)
		return 0;
	for (size_t i = 0u; i <= g_http_rx_len; i++) buf[i] = g_http_rx[i];
	*len = g_http_rx_len;
	g_http_rx_len = 0u;
	g_http_ready = 0;
	return 1;
}

void mcufw_net_lwip_http_tx(const char *buf, size_t len)
{
	if (!g_http_conn || len == 0u) return;
	if (tcp_sndbuf(g_http_conn) < (u16_t)len) return;
	if (tcp_write(g_http_conn, buf, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) return;
	(void)tcp_output(g_http_conn);
}

/* --------------------------------------------------------------- binding -- */
int mcufw_net_lwip_bound(void)
{
	return g_bound;
}

int mcufw_net_lwip_bind(void)
{
	if (g_bound)
		return 0;

	if (!mcufw_lwip_ready())
		mcufw_lwip_init();

	IP4_ADDR(&g_any_addr, 0, 0, 0, 0);

	g_udp_pcb = udp_new();
	if (g_udp_pcb) {
		if (udp_bind(g_udp_pcb, &g_any_addr,
			     (u16_t)MCUFW_MIIO_PORT) != ERR_OK) {
			udp_remove(g_udp_pcb);
			g_udp_pcb = NULL;
		} else {
			udp_recv(g_udp_pcb, udp_rx_cb, NULL);
		}
	}

	g_listen_pcb = tcp_new();
	if (g_listen_pcb) {
		if (tcp_bind(g_listen_pcb, &g_any_addr,
			     (u16_t)MCUFW_LAN_RPC_PORT) != ERR_OK) {
			(void)tcp_close(g_listen_pcb);
			g_listen_pcb = NULL;
		} else {
			g_listen_pcb = tcp_listen_with_backlog(g_listen_pcb, 1);
			if (g_listen_pcb) {
				tcp_arg(g_listen_pcb, NULL);
				tcp_accept(g_listen_pcb, tcp_accept_cb);
			}
		}
	}

	g_http_listen = tcp_new();
	if (g_http_listen) {
		if (tcp_bind(g_http_listen, &g_any_addr, (u16_t)MCUFW_HTTP_PORT) != ERR_OK) {
			(void)tcp_close(g_http_listen);
			g_http_listen = NULL;
		} else {
			g_http_listen = tcp_listen_with_backlog(g_http_listen, 1);
			if (g_http_listen) {
				tcp_arg(g_http_listen, NULL);
				tcp_accept(g_http_listen, http_accept_cb);
			}
		}
	}

	g_bound = (g_udp_pcb != NULL && g_listen_pcb != NULL) ? 1 : 0;
	return g_bound ? 0 : -1;
}

static const mcufw_net_transport_t g_lwip_tr = {
	lwip_udp_recv,
	lwip_udp_send,
	lwip_tcp_recv,
	lwip_tcp_send
};

const mcufw_net_transport_t *mcufw_net_lwip_transport(void)
{
	return &g_lwip_tr;
}
