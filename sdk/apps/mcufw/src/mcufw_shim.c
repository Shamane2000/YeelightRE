/*
 * mcufw_shim.c -- phase 3.5: reduced to libc stubs only.
 *
 * All FreeRTOS/lwIP shims from phase 3.4 are GONE. The real implementations
 * now come from:
 *   - FreeRTOS: tasks.c, queue.c, timers.c, heap_4.c, port.c (ARM_CM4F)
 *   - lwIP: core sources, sys_arch.c (wmsdk port)
 *   - MQTT: AWS embedded client
 *
 * What remains here:
 *   1. memcpy/memmove/memset -- freestanding builds have no libc; vendor
 *      code compiled against newlib references these.
 *   2. __ctype_ptr__ -- newlib ctype table access point.
 *
 * vPortMalloc/vPortFree/pvPortReAlloc are now provided by heap_4.c.
 * All task/queue/timer/netif symbols are provided by the real SDK sources.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d == s || n == 0u)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = (uint8_t *)dst;

	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

const unsigned char __ctype_ptr__[384];

/* Phase 3.5a: Stubs for symbols expected by libwmsdk_mcufw.a but missing
 * in our minimal lwIP build (-nostdlib, no full WMSDK stack).
 * These are never called at runtime in Phase 3.5a (no socket API usage yet). */
#include <errno.h>

int *__errno(void) {
    static int shim_errno = 0;
    return &shim_errno;
}

void stats_display(void) { /* stub */ }
void stats_sock_display(void) { /* stub */ }

/* Phase 3.5a: Socket-API stubs for libwmsdk_mcufw.a.
 * We use raw lwIP API only; the BSD socket layer is not linked.
 * These are never called at runtime in this phase. */
void lwip_socket_init(void) { /* stub */ }
int lwip_getpeername(int s, void *name, void *namelen) { (void)s; (void)name; (void)namelen; return -1; }
