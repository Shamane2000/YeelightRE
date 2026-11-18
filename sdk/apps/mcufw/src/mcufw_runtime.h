/*
 * mcufw_runtime.h -- the runtime service loop (phase 3.8).
 *
 * Replaces the endless boot ramp of 3.7.  One tick does, in this order:
 *
 *   1. board-out + button diagnostics (idempotent reads, no blocking)
 *   2. one bounded, non-blocking pump per network channel
 *        UDP 54321 -> mcufw_miio_handle()
 *        TCP 55443 -> mcufw_rpc_parse()/mcufw_rpc_dispatch()
 *   3. mcufw_pwm_service(): consume the dirty flag.  A tick without a state
 *      change issues ZERO GPT0 register writes -- mcufw_lamp_set() is never
 *      called here, so mcufw_pwm stays the single writer on CMR0.
 *   4. every MCUFW_RUNTIME_STATUS_EVERY ticks: one UART status line with the
 *      real counters (ticks, sampled pins, pumps, replies, reg writes).
 *
 * No busy_delay() anywhere in the loop; the delay between iterations belongs
 * to the caller (target: vTaskDelay / tick source), not to a spin loop.
 */
#ifndef MCUFW_RUNTIME_H
#define MCUFW_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/* One status line every 32 ticks in the host test; the target overrides this
 * with -DMCUFW_RUNTIME_STATUS_EVERY=<n> (no RTOS tick source yet, so the tick
 * counter is the only clock there and 32 would flood the UART). */
#ifndef MCUFW_RUNTIME_STATUS_EVERY
#define MCUFW_RUNTIME_STATUS_EVERY 32u
#endif
#define MCUFW_RUNTIME_STATUS_MAX   128u

typedef struct mcufw_runtime_io {
	int  (*board_out_read)(void); /* board-out pin level                */
	int  (*button_read)(void);    /* button pin level, -1 = none on board */
	void (*status_write)(const char *line); /* UART line sink            */
} mcufw_runtime_io_t;

typedef struct {
	uint32_t ticks;            /* mcufw_runtime_tick() calls            */
	uint32_t out_reads;        /* board-out samples                     */
	uint32_t button_reads;     /* button samples                        */
	uint32_t status_lines;     /* status lines emitted                  */
	uint32_t net_udp_pumps;    /* UDP pumps issued by the loop          */
	uint32_t net_tcp_pumps;    /* TCP pumps issued by the loop          */
	uint32_t net_udp_datagrams;/* datagrams dispatched (from net stats) */
	uint32_t net_tcp_requests; /* request lines dispatched              */
	uint32_t idle_ticks;       /* ticks with 0 register writes          */
	uint32_t write_ticks;      /* ticks that observed a dirty pwm       */
	uint32_t reg_writes_total; /* GPT0 writes attributed to the loop    */
	uint32_t last_tick_writes; /* writes of the most recent tick        */
	uint32_t config_saves;     /* ticks that persisted a NEW state (ph.4)*/
	uint8_t  last_out;         /* last sampled board-out level          */
	int      last_button;      /* last sampled button level, -1 = none  */
	char     last_status[MCUFW_RUNTIME_STATUS_MAX];
} mcufw_runtime_stats_t;

/* Plug in the board I/O and zero the counters. */
void mcufw_runtime_init(const mcufw_runtime_io_t *io);

/* One service tick.  Never blocks, never writes GPT0 unless the state path
 * marked the channels dirty. */
void mcufw_runtime_tick(void);

const mcufw_runtime_stats_t *mcufw_runtime_stats(void);

#endif /* MCUFW_RUNTIME_H */
