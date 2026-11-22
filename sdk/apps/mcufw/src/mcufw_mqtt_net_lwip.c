/*
 * mcufw_mqtt_net_lwip.c -- lwIP 1.4.1 non-blocking raw TCP + DNS adapter.
 *
 * Uses raw tcp_pcb callbacks decoupled through a 1024-byte RX ring buffer so
 * mcufw_mqtt_client_tick() drives all MQTT logic strictly on the runtime_task
 * single-writer thread.
 */
#include "mcufw_mqtt_net_lwip.h"
#include "mcufw_wlan.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include "FreeRTOS.h"
#include "task.h"

#define LWIP_MQTT_RX_CAP 1024u

enum {
	LWIP_MQTT_CLOSED     = 0,
	LWIP_MQTT_CONNECTING = 1,
	LWIP_MQTT_CONNECTED  = 2,
};

static struct tcp_pcb *g_pcb;
static volatile int    g_state = LWIP_MQTT_CLOSED;
static uint16_t        g_target_port;

static uint8_t         g_rx_buf[LWIP_MQTT_RX_CAP];
static unsigned        g_rx_rd;
static unsigned        g_rx_wr;

static void rx_reset(void)
{
	g_rx_rd = 0u;
	g_rx_wr = 0u;
}

static void rx_push_bytes(const uint8_t *d, u16_t len)
{
	for (u16_t i = 0u; i < len; i++) {
		unsigned next = (g_rx_wr + 1u) % LWIP_MQTT_RX_CAP;
		if (next == g_rx_rd)
			break;
		g_rx_buf[g_rx_wr] = d[i];
		g_rx_wr = next;
	}
}

static void mqtt_err_cb(void *arg, err_t err)
{
	(void)arg;
	(void)err;
	/* lwIP already freed the PCB before invoking tcp_err callback. */
	g_pcb = NULL;
	g_state = LWIP_MQTT_CLOSED;
}

static err_t mqtt_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)arg;

	if (!p) {
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
		tcp_arg(pcb, NULL);
		if (pcb == g_pcb)
			g_pcb = NULL;
		g_state = LWIP_MQTT_CLOSED;
		if (err == ERR_OK)
			(void)tcp_close(pcb);
		return ERR_OK;
	}

	if (p->tot_len > 0u) {
		for (struct pbuf *q = p; q != NULL; q = q->next)
			rx_push_bytes((const uint8_t *)q->payload, q->len);
		tcp_recved(pcb, p->tot_len);
	}
	pbuf_free(p);
	return ERR_OK;
}

static err_t mqtt_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
	(void)arg;

	if (pcb != g_pcb)
		return ERR_OK;
	if (err != ERR_OK) {
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
		tcp_arg(pcb, NULL);
		g_pcb = NULL;
		g_state = LWIP_MQTT_CLOSED;
		(void)tcp_close(pcb);
		return ERR_OK;
	}
	tcp_recv(pcb, mqtt_recv_cb);
	tcp_nagle_disable(pcb);
	g_state = LWIP_MQTT_CONNECTED;
	return ERR_OK;
}

static int start_tcp_connect(ip_addr_t *addr, uint16_t port)
{
	struct tcp_pcb *pcb;

	if (g_pcb) {
		tcp_recv(g_pcb, NULL);
		tcp_err(g_pcb, NULL);
		tcp_arg(g_pcb, NULL);
		tcp_abort(g_pcb);
		g_pcb = NULL;
	}
	rx_reset();

	pcb = tcp_new();
	if (!pcb) {
		g_state = LWIP_MQTT_CLOSED;
		return -1;
	}
	g_pcb = pcb;
	g_state = LWIP_MQTT_CONNECTING;
	tcp_arg(pcb, NULL);
	tcp_err(pcb, mqtt_err_cb);
	if (tcp_connect(pcb, addr, port, mqtt_connected_cb) != ERR_OK) {
		tcp_err(pcb, NULL);
		tcp_arg(pcb, NULL);
		tcp_abort(pcb);
		g_pcb = NULL;
		g_state = LWIP_MQTT_CLOSED;
		return -1;
	}
	return 0;
}

static void mqtt_dns_cb(const char *name, ip_addr_t *ipaddr, void *arg)
{
	(void)name;
	(void)arg;

	if (g_state != LWIP_MQTT_CONNECTING || g_pcb != NULL)
		return;
	if (!ipaddr || start_tcp_connect(ipaddr, g_target_port) != 0)
		g_state = LWIP_MQTT_CLOSED;
}

static int lwip_link_ready(void)
{
	return mcufw_wlan_get_state() == MCUFW_WLAN_STATE_STA_CONNECTED ? 1 : 0;
}

static int lwip_tcp_connect(const char *host, uint16_t port)
{
	ip_addr_t addr;
	err_t rc;

	if (!host || !host[0] || port == 0u)
		return -1;
	rx_reset();
	g_target_port = port;

	if (ipaddr_aton(host, &addr))
		return start_tcp_connect(&addr, port);

	g_state = LWIP_MQTT_CONNECTING;
	rc = dns_gethostbyname(host, &addr, mqtt_dns_cb, NULL);
	if (rc == ERR_OK)
		return start_tcp_connect(&addr, port);
	if (rc == ERR_INPROGRESS)
		return 0;
	g_state = LWIP_MQTT_CLOSED;
	return -1;
}

static int lwip_tcp_state(void)
{
	return g_state;
}

static int lwip_tcp_send(const uint8_t *buf, size_t len)
{
	u16_t avail;
	u16_t chunk;
	err_t rc;

	if (!g_pcb || g_state != LWIP_MQTT_CONNECTED)
		return -1;
	if (len == 0u)
		return 0;

	avail = tcp_sndbuf(g_pcb);
	if (avail == 0u)
		return 0;

	chunk = (len < (size_t)avail) ? (u16_t)len : avail;
	rc = tcp_write(g_pcb, buf, chunk, TCP_WRITE_FLAG_COPY);
	if (rc == ERR_MEM)
		return 0;
	if (rc != ERR_OK)
		return -1;
	(void)tcp_output(g_pcb);
	return (int)chunk;
}

static int lwip_tcp_recv(uint8_t *buf, size_t cap)
{
	size_t n = 0u;

	if (!buf || cap == 0u)
		return 0;
	while (n < cap && g_rx_rd != g_rx_wr) {
		buf[n++] = g_rx_buf[g_rx_rd];
		g_rx_rd = (g_rx_rd + 1u) % LWIP_MQTT_RX_CAP;
	}
	if (n == 0u && g_state == LWIP_MQTT_CLOSED)
		return -1;
	return (int)n;
}

static void lwip_tcp_close(void)
{
	if (g_pcb) {
		struct tcp_pcb *pcb = g_pcb;
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
		tcp_arg(pcb, NULL);
		g_pcb = NULL;
		if (tcp_close(pcb) != ERR_OK)
			tcp_abort(pcb);
	}
	g_state = LWIP_MQTT_CLOSED;
	rx_reset();
}

static uint32_t lwip_now_ms(void)
{
	return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

static void fmt_u8(char *dst, size_t cap, size_t *pos, uint8_t v)
{
	char tmp[4];
	int i = 0;

	do {
		tmp[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	} while (v && i < 3);
	while (i-- > 0 && *pos + 1u < cap)
		dst[(*pos)++] = tmp[i];
	dst[*pos] = '\0';
}

static int lwip_diag_info(char *ip_out, size_t ip_cap, int *rssi_out,
                          uint8_t *active_slot_out)
{
	if (ip_out && ip_cap > 0u) {
		if (netif_default != NULL && netif_default->ip_addr.addr != 0u) {
			const ip_addr_t *ip = &netif_default->ip_addr;
			size_t pos = 0u;
			fmt_u8(ip_out, ip_cap, &pos, ip4_addr1(ip));
			if (pos + 1u < ip_cap) ip_out[pos++] = '.';
			fmt_u8(ip_out, ip_cap, &pos, ip4_addr2(ip));
			if (pos + 1u < ip_cap) ip_out[pos++] = '.';
			fmt_u8(ip_out, ip_cap, &pos, ip4_addr3(ip));
			if (pos + 1u < ip_cap) ip_out[pos++] = '.';
			fmt_u8(ip_out, ip_cap, &pos, ip4_addr4(ip));
		} else {
			static const char z[] = "0.0.0.0";
			size_t i = 0u;
			while (z[i] && i + 1u < ip_cap) {
				ip_out[i] = z[i];
				i++;
			}
			ip_out[i] = '\0';
		}
	}
	if (rssi_out)
		*rssi_out = 0;
	if (active_slot_out)
		*active_slot_out = 0u;
	return 0;
}

static const mcufw_mqtt_io_t g_lwip_mqtt_io = {
	lwip_link_ready,
	lwip_tcp_connect,
	lwip_tcp_state,
	lwip_tcp_send,
	lwip_tcp_recv,
	lwip_tcp_close,
	lwip_now_ms,
	lwip_diag_info,
};

const mcufw_mqtt_io_t *mcufw_mqtt_lwip_io(void)
{
	return &g_lwip_mqtt_io;
}
