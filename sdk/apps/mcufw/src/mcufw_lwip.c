/*
 * mcufw_lwip.c -- lwIP stack bring-up for mcufw phase 3.5.
 *
 * Initialises the TCP/IP stack on top of FreeRTOS (sys_arch from the SDK
 * port). The netif is registered but not brought up yet -- AP join is
 * phase 5. This file provides the socket layer that mcufw_miio.c,
 * mcufw_lan_rpc.c and mcufw_mqtt.c bind to.
 *
 * NOTE: SDK uses lwIP 1.4.x API (ip_addr_t, not ip4_addr_t).
 */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "netif/etharp.h"
#include <stdint.h>

static struct netif mcufw_netif;
static volatile int lwip_ready_flag;

int mcufw_lwip_ready(void)
{
	return lwip_ready_flag;
}

struct netif *mcufw_lwip_netif(void)
{
	return &mcufw_netif;
}

/* Called once from the startup task before any socket use. */
void mcufw_lwip_init(void)
{
	lwip_init();
	/* Netif registration without IP -- DHCP or static config comes later
	 * (phase 5 AP join). The interface exists so UDP/TCP binds succeed
	 * against INADDR_ANY. lwIP 1.4.x uses ip_addr_t. */
	ip_addr_t ip, mask, gw;
	IP4_ADDR(&ip, 0, 0, 0, 0);
	IP4_ADDR(&mask, 255, 255, 255, 0);
	IP4_ADDR(&gw, 0, 0, 0, 0);
	netif_add(&mcufw_netif, &ip, &mask, &gw, NULL, NULL, tcpip_input);
	netif_set_default(&mcufw_netif);
	lwip_ready_flag = 1;
}
