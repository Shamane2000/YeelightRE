/* sdk_compat.h — Provide WMSDK board-level defines expected by vendor
 * lwipopts.h. Force-included via -include in SDK_CFLAGS before any lwIP
 * header is processed. Values match vendor autoconf.h for MW300/WiFi8801.
 */
#ifndef MCUFW_SDK_COMPAT_H
#define MCUFW_SDK_COMPAT_H

/* lwipopts.h references these without CONFIG_ prefix; the real values come
 * from the WMSDK Kconfig but we cannot include autoconf.h here because its
 * search path is not set up for the force-include context. */
#define MAX_SOCKETS_TCP             8
#define MAX_LISTENING_SOCKETS_TCP   4
#define MAX_SOCKETS_UDP             6
#define TCPIP_STACK_TX_HEAP_SIZE    24

#endif /* MCUFW_SDK_COMPAT_H */
