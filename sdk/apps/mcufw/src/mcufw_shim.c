/*
 * mcufw_shim.c -- link-time shims for the pieces of the WLAN stack that are
 * pulled in by mlan_init.o / mdev_startup.o but are not part of phase 3.4.
 *
 * What this file is NOT: a FreeRTOS or a lwIP implementation.  Nothing here
 * schedules anything, and nothing on the 3.4 code path calls into it -- the
 * vendor code that would call it (wlan_init_adapter -> os_init -> kernel,
 * mlan -> dhcp/netif) only runs once mcufw_os_ready() returns 1.  These
 * definitions exist so that the *whole* mlan/mdev object graph links with
 * zero undefined symbols, i.e. so that the phase-3.4 acceptance ("WLAN
 * symbols linked, 0 undefined") says something real about the SDK surface we
 * still have to fill in phase 5.
 *
 * The one functional piece is pvPortMalloc/pvPortFree: a bump allocator over
 * the linker-provided heap window, so a future caller cannot silently get
 * NULL from an empty stub.  free() is a no-op -- deliberate, documented, and
 * only reachable in phase 5.
 *
 * Every symbol here is listed in docs/10 §4 with the archive member that
 * needs it and the milestone that replaces the shim.
 */
#include <stddef.h>
#include <stdint.h>

typedef void *task_h;
typedef void *queue_h;
typedef void *timer_h;

#define UNUSED(x) ((void)(x))
#define SHIM_PASS 1        /* pdPASS */
#define SHIM_FAIL 0        /* pdFAIL */

/* ------------------------------------------------------------- RTOS state -- */
/* The scheduler is not part of 3.4; mcufw_wlan_bringup() asks this gate before
 * it would touch the SDK.  Returning 1 here is the phase-5 switch. */
int mcufw_os_ready(void)
{
	return 0;
}

/* ------------------------------------------------------------------- heap -- */
extern uint8_t _heap_2_start[];
extern uint8_t _heap_2_end[];

static uintptr_t heap_top;            /* 0 => not initialised yet */
static uint32_t heap_used;

static uintptr_t heap_base(void)
{
	if (heap_top == 0u)
		heap_top = ((uintptr_t)_heap_2_start + 7u) & ~(uintptr_t)7u;
	return heap_top;
}

void *pvPortMalloc(size_t size)
{
	uintptr_t base = heap_base();
	uintptr_t end = (uintptr_t)_heap_2_end;
	uintptr_t next = (base + heap_used + 7u) & ~(uintptr_t)7u;

	if (size == 0u || next + size > end)
		return NULL;
	heap_used = (uint32_t)(next + size - base);
	return (void *)next;
}

void *pvPortReAlloc(void *ptr, size_t size)
{
	void *fresh;
	size_t old = 0;

	if (!ptr)
		return pvPortMalloc(size);
	if (size == 0u)
		return NULL;

	/* Header-less allocator: the old size is unknown, so copy conservatively
	 * up to `size` bytes from the old block (documented phase-5 limitation). */
	fresh = pvPortMalloc(size);
	if (!fresh)
		return NULL;
	for (old = 0; old < size; old++) {
		uint8_t *src = (uint8_t *)ptr + old;

		if ((uintptr_t)src >= (uintptr_t)_heap_2_end)
			break;
		((uint8_t *)fresh)[old] = *src;
	}
	return fresh;
}

void vPortFree(void *ptr)
{
	UNUSED(ptr);            /* bump allocator: no reclaim (phase 5) */
}

/* Returns the number of bytes the bank added (0 for the shim). */
uint32_t prvHeapAddMemBank(uint8_t *start, uint8_t *end)
{
	UNUSED(start);
	UNUSED(end);
	return 0u;
}

/* --------------------------------------------------------------- critical -- */
void vPortEnterCritical(void) { }
void vPortExitCritical(void) { }
void vPortYield(void) { }

/* ------------------------------------------------------------------ tasks -- */
static uint32_t idle_task_stub;

void vTaskDelay(uint32_t ticks) { UNUSED(ticks); }
void vTaskDelete(task_h task) { UNUSED(task); }
void vTaskStartScheduler(void) { }
void vTaskSuspend(task_h task) { UNUSED(task); }

int xTaskGenericCreate(void *entry, const char *name, uint32_t stack_depth,
		       void *param, uint32_t prio, task_h *created,
		       void *stack, uint32_t stack_size)
{
	UNUSED(entry); UNUSED(name); UNUSED(stack_depth); UNUSED(param);
	UNUSED(prio); UNUSED(stack); UNUSED(stack_size);
	if (created)
		*created = &idle_task_stub;
	return SHIM_PASS;
}

task_h xTaskGetCurrentTaskHandle(void)
{
	return &idle_task_stub;
}

const char *pcTaskGetTaskName(task_h task)
{
	UNUSED(task);
	return "mcufw-shim";
}

uint32_t xTaskGetTickCount(void) { return 0u; }
uint32_t xTaskGetTickCountFromISR(void) { return 0u; }
uint32_t xTaskGetTotalTickCount(void) { return 0u; }
uint32_t xTaskGetTotalTickCountFromISR(void) { return 0u; }
uint32_t xTaskGetUnblockTime(void) { return 0u; }
void xTaskUpdateTickCount(uint32_t ticks) { UNUSED(ticks); }

/* ----------------------------------------------------------------- queues -- */
static uint32_t queue_stub;

queue_h xQueueGenericCreate(uint32_t len, uint32_t item_size, uint8_t type)
{
	UNUSED(len); UNUSED(item_size); UNUSED(type);
	return &queue_stub;
}

queue_h xQueueCreateCountingSemaphore(uint32_t max, uint32_t initial)
{
	UNUSED(max); UNUSED(initial);
	return &queue_stub;
}

queue_h xQueueCreateMutex(uint8_t type)
{
	UNUSED(type);
	return &queue_stub;
}

int xQueueGenericSend(queue_h q, const void *item, uint32_t wait, uint8_t pos)
{
	UNUSED(q); UNUSED(item); UNUSED(wait); UNUSED(pos);
	return SHIM_PASS;
}

int xQueueGenericReceive(queue_h q, void *item, uint32_t wait, uint8_t peek)
{
	UNUSED(q); UNUSED(item); UNUSED(wait); UNUSED(peek);
	return SHIM_FAIL;
}

int xQueueGenericSendFromISR(queue_h q, const void *item, int32_t *woken,
			     uint8_t pos)
{
	UNUSED(q); UNUSED(item); UNUSED(pos);
	if (woken)
		*woken = 0;
	return SHIM_PASS;
}

int xQueueGiveFromISR(queue_h q, int32_t *woken)
{
	UNUSED(q);
	if (woken)
		*woken = 0;
	return SHIM_PASS;
}

int xQueueReceiveFromISR(queue_h q, void *item, int32_t *woken)
{
	UNUSED(q); UNUSED(item);
	if (woken)
		*woken = 0;
	return SHIM_FAIL;
}

uint32_t uxQueueMessagesWaiting(queue_h q)
{
	UNUSED(q);
	return 0u;
}

void vQueueDelete(queue_h q) { UNUSED(q); }

/* ----------------------------------------------------------------- timers -- */
timer_h xTimerCreate(const char *name, uint32_t period, uint32_t reload,
		     void *timer_id, void *callback)
{
	UNUSED(name); UNUSED(period); UNUSED(reload); UNUSED(timer_id);
	UNUSED(callback);
	return &queue_stub;
}

int xTimerGenericCommand(timer_h t, uint8_t command, uint32_t opt,
			 int32_t *woken, uint32_t wait)
{
	UNUSED(t); UNUSED(command); UNUSED(opt); UNUSED(wait);
	if (woken)
		*woken = 0;
	return SHIM_PASS;
}

void *pvTimerGetTimerID(timer_h t)
{
	UNUSED(t);
	return NULL;
}

/* ------------------------------------------------------------------- lwIP -- *
 * netif/dhcp/dns/pbuf/tcpip symbols pulled in by mlan's net path.  All are
 * inert; the real lwIP core is phase 5.
 */
void *mem_malloc(size_t size) { return pvPortMalloc(size); }

int  dhcp_start(void *netif) { UNUSED(netif); return SHIM_FAIL; }
void dhcp_stop(void *netif) { UNUSED(netif); }

void *dns_getserver(uint8_t num) { UNUSED(num); return NULL; }
void dns_setserver(uint8_t num, const void *addr) { UNUSED(num); UNUSED(addr); }

int etharp_output(void *netif, void *p, const void *ipaddr)
{
	UNUSED(netif); UNUSED(p); UNUSED(ipaddr);
	return SHIM_FAIL;
}

void netif_set_default(void *netif) { UNUSED(netif); }
void netif_set_down(void *netif) { UNUSED(netif); }
void netif_set_up(void *netif) { UNUSED(netif); }
void netif_set_status_callback(void *netif, void *cb) { UNUSED(netif); UNUSED(cb); }

int netifapi_netif_add(void *netif, const void *ip, const void *mask,
		       const void *gw, void *state, void *init, void *input)
{
	UNUSED(netif); UNUSED(ip); UNUSED(mask); UNUSED(gw); UNUSED(state);
	UNUSED(init); UNUSED(input);
	return SHIM_FAIL;
}

int netifapi_netif_set_addr(void *netif, const void *ip, const void *mask,
			    const void *gw)
{
	UNUSED(netif); UNUSED(ip); UNUSED(mask); UNUSED(gw);
	return SHIM_FAIL;
}

int netifapi_netif_common(void *netif, void *fn, void *arg)
{
	UNUSED(netif); UNUSED(fn); UNUSED(arg);
	return SHIM_FAIL;
}

void *pbuf_alloc(uint32_t layer, uint16_t length, uint32_t type)
{
	UNUSED(layer); UNUSED(length); UNUSED(type);
	return NULL;
}

void pbuf_free(void *p) { UNUSED(p); }

int lwip_getpeername(int s, void *name, void *namelen)
{
	UNUSED(s); UNUSED(name); UNUSED(namelen);
	return -1;
}

char *ipaddr_ntoa(const void *addr)
{
	static char buf[16] = "0.0.0.0";

	UNUSED(addr);
	return buf;
}

void tcpip_init(void *init, void *arg) { UNUSED(init); UNUSED(arg); }

int tcpip_input(void *p, void *inp)
{
	UNUSED(p); UNUSED(inp);
	return SHIM_FAIL;
}

void stats_display(void) { }
void stats_sock_display(void) { }

/* lwip_stats is a data symbol (struct stats_); a zeroed 512-byte stand-in is
 * enough for the code that would only ever read counters. */
uint8_t lwip_stats[512];

/* ------------------------------------------------------------------ libc --- *
 * The firmware is -nostdlib; vendor code was compiled against newlib and
 * references these.  memcpy/memset are the hot ones -- kept out of the shim's
 * "inert" category, they are real (bounded, no overlap assumption for memcpy).
 */
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

/* newlib's ctype table access point (vendor code compiled with ctype macros). */
const unsigned char __ctype_ptr__[384];
