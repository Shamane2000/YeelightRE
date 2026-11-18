/*
 * mcufw_net_lwip.h -- raw lwIP transport for the runtime service loop.
 *
 * Target-only (not part of the host build): binds UDP 54321 and listens on
 * TCP 55443 with the lwIP 1.4.x raw API and hands the received bytes to
 * mcufw_net.c through the mcufw_net_transport_t seam.
 *
 * The BSD socket layer is not linked in this image (see mcufw_miio.h), so the
 * raw PCB callbacks are used.  They run in the tcpip thread context and only
 * ever fill a small FIFO -- all parsing and register work stays in the loop,
 * which keeps mcufw_state/mcufw_pwm single-threaded (single writer).
 */
#ifndef MCUFW_NET_LWIP_H
#define MCUFW_NET_LWIP_H

#include "mcufw_net.h"

/* Bring lwIP up (idempotent) and bind/listen on both lamp ports.
 * Returns 0 when both ports are owned by this image, -1 otherwise. */
int mcufw_net_lwip_bind(void);

/* 1 = UDP 54321 and TCP 55443 are bound and listening. */
int mcufw_net_lwip_bound(void);

/* Transport for mcufw_net_init(). */
const mcufw_net_transport_t *mcufw_net_lwip_transport(void);

#endif /* MCUFW_NET_LWIP_H */
