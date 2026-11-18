/*
 * mcufw_runtime.c -- runtime service loop (phase 3.8).
 *
 * Ownership rule this file implements (see docs + mcufw_gpt.h):
 *
 *   mcufw_lamp_set()  -- boot diagnosis ONLY (main.c, bounded sweep)
 *   mcufw_state/mcufw_pwm -- the ONLY writer on GPT0 CMR0 in the steady state
 *
 * The loop therefore never programs a channel itself.  It samples the board
 * pins, pumps both network front-ends non-blockingly, and lets mcufw_pwm
 * consume its dirty flag.  Consequences:
 *
 *   - a state change from UDP or TCP survives every tick (no ramp overwrite)
 *   - a tick without a state change performs 0 register writes
 *   - the UART status line reports the CMR0 word read back through the same
 *     HAL seam the driver writes through, so the log cannot lie about it.
 */
#include "mcufw_pwm.h"
#include "mcufw_net.h"
#include "mcufw_regs.h"
#include "mcufw_runtime.h"
#include "mcufw_config.h"
#include "mcufw_state.h"

static mcufw_runtime_io_t g_io;
static mcufw_runtime_stats_t g_rt;

/* ------------------------------------------------------------- formatting -- */
static void str_put(char *dst, size_t cap, size_t *pos, const char *s)
{
	while (*s && *pos + 1u < cap)
		dst[(*pos)++] = *s++;
	dst[*pos] = '\0';
}

static void num_put(char *dst, size_t cap, size_t *pos, uint32_t v)
{
	char tmp[10];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	} while (v && n < (int)sizeof(tmp));
	while (n > 0 && *pos + 1u < cap)
		dst[(*pos)++] = tmp[--n];
	dst[*pos] = '\0';
}

static void hex_put(char *dst, size_t cap, size_t *pos, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	str_put(dst, cap, pos, "0x");
	for (shift = 28; shift >= 0; shift -= 4) {
		if (*pos + 1u >= cap)
			break;
		dst[(*pos)++] = digits[(v >> (unsigned)shift) & 0xfu];
	}
	dst[*pos] = '\0';
}

/* ------------------------------------------------------------ status line -- */
static void status_line(void)
{
	const mcufw_pwm_stats_t *pwm = mcufw_pwm_stats();
	char *dst = g_rt.last_status;
	size_t pos = 0;

	str_put(dst, sizeof(g_rt.last_status), &pos, "mcufw loop ");
	hex_put(dst, sizeof(g_rt.last_status), &pos, g_rt.ticks);
	str_put(dst, sizeof(g_rt.last_status), &pos, " lamp ");
	num_put(dst, sizeof(g_rt.last_status), &pos, (uint32_t)pwm->white_level);
	str_put(dst, sizeof(g_rt.last_status), &pos, " color ");
	num_put(dst, sizeof(g_rt.last_status), &pos, (uint32_t)pwm->color_level);
	str_put(dst, sizeof(g_rt.last_status), &pos, " cmr0 ");
	hex_put(dst, sizeof(g_rt.last_status), &pos,
		mcufw_pwm_cmr0(MCUFW_PWM_CH_WHITE0));
	str_put(dst, sizeof(g_rt.last_status), &pos, " out ");
	num_put(dst, sizeof(g_rt.last_status), &pos, (uint32_t)g_rt.last_out);
	str_put(dst, sizeof(g_rt.last_status), &pos, " udp ");
	num_put(dst, sizeof(g_rt.last_status), &pos, g_rt.net_udp_datagrams);
	str_put(dst, sizeof(g_rt.last_status), &pos, " tcp ");
	num_put(dst, sizeof(g_rt.last_status), &pos, g_rt.net_tcp_requests);
	str_put(dst, sizeof(g_rt.last_status), &pos, " wr ");
	num_put(dst, sizeof(g_rt.last_status), &pos, g_rt.reg_writes_total);

	if (g_io.status_write)
		g_io.status_write(dst);
	g_rt.status_lines++;
}

/* --------------------------------------------------------------- the loop -- */
void mcufw_runtime_init(const mcufw_runtime_io_t *io)
{
	g_io.board_out_read = io ? io->board_out_read : 0;
	g_io.button_read = io ? io->button_read : 0;
	g_io.status_write = io ? io->status_write : 0;

	g_rt.ticks = 0u;
	g_rt.out_reads = 0u;
	g_rt.button_reads = 0u;
	g_rt.status_lines = 0u;
	g_rt.net_udp_pumps = 0u;
	g_rt.net_tcp_pumps = 0u;
	g_rt.net_udp_datagrams = 0u;
	g_rt.net_tcp_requests = 0u;
	g_rt.idle_ticks = 0u;
	g_rt.write_ticks = 0u;
	g_rt.reg_writes_total = 0u;
	g_rt.last_tick_writes = 0u;
	g_rt.config_saves = 0u;
	g_rt.last_out = 0u;
	g_rt.last_button = -1;
	g_rt.last_status[0] = '\0';
}

void mcufw_runtime_tick(void)
{
	const mcufw_pwm_stats_t *pwm = mcufw_pwm_stats();
	uint32_t before = pwm->reg_writes;
	uint32_t after;

	g_rt.ticks++;

	/* 1. board diagnostics ------------------------------------------------- */
	if (g_io.board_out_read) {
		g_rt.last_out = (uint8_t)(g_io.board_out_read() ? 1 : 0);
		g_rt.out_reads++;
	}
	if (g_io.button_read) {
		int b = g_io.button_read();

		g_rt.last_button = b;
		g_rt.button_reads++;
	}

	/* 2. network channels, bounded and non-blocking ------------------------ */
	mcufw_net_pump_udp();
	mcufw_net_pump_tcp();
	{
		/* Mirror the transport counters instead of counting the pump's
		 * return value (which is "replies sent", not "datagrams read"). */
		const mcufw_net_stats_t *net = mcufw_net_stats();

		g_rt.net_udp_pumps = net->udp_pumps;
		g_rt.net_tcp_pumps = net->tcp_pumps;
		g_rt.net_udp_datagrams = net->udp_datagrams;
		g_rt.net_tcp_requests = net->tcp_requests;
	}

	/* 3. hand the tick to the single writer -------------------------------- */
	if (mcufw_pwm_service())
		g_rt.write_ticks++;
	else
		g_rt.idle_ticks++;

	/* 3b. Persistenz: nur bei einer neuen State-Mutation, nie pro Idle-Tick.
	 * mcufw_config_service() vergleicht den Mutation-Zaehler und ueberspringt
	 * zudem byte-identische Bloecke. */
	if (mcufw_config_service(mcufw_state()))
		g_rt.config_saves++;

	after = mcufw_pwm_stats()->reg_writes;
	g_rt.last_tick_writes = after - before;
	g_rt.reg_writes_total += g_rt.last_tick_writes;

	/* 4. periodic status --------------------------------------------------- */
	if ((g_rt.ticks % MCUFW_RUNTIME_STATUS_EVERY) == 0u)
		status_line();
}

const mcufw_runtime_stats_t *mcufw_runtime_stats(void)
{
	return &g_rt;
}
