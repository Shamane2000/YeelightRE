/* main.c — mcufw phase 4: persistente Config + Power-on-Verhalten.
 *
 * New in 3.6:
 *   - mcufw_state: single lamp state (power/bright/ct/hsv/name/cf/default)
 *     with range clamping, mutated by both network front-ends
 *   - mcufw_pwm: PWM backend seam (records mode/duty; real GPT writes = ph.4)
 *   - LAN-RPC setters mutate the state and answer the clamped value
 *   - miIO set_ps/get_ps + miIO.info read/write the same state
 *
 * Phase 3.5 kept: FreeRTOS scheduler, lwIP stack (no AP join), LAN JSON-RPC
 * whitelist, MQTT discovery payloads, Color Flow dispatch skeleton.
 */
#include "mcufw_regs.h"
#include "mcufw_gpt.h"
#include "mcufw_wlan.h"
#include "mcufw_miio.h"
#include "mcufw_crypto.h"
#include "mcufw_selftest.h"
#include "mcufw_lan_rpc.h"
#include "mcufw_mqtt.h"
#include "mcufw_cf.h"
#include "mcufw_pwm.h"
#include "mcufw_state.h"
#include "mcufw_net.h"
#include "mcufw_net_lwip.h"
#include "mcufw_runtime.h"
#include "mcufw_config.h"
#include "mcufw_config_hal.h"
#include "mcufw_rollback.h"

#ifndef HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
extern void mcufw_rtos_start(void);
#endif

#define MCUFW_VERSION "mcufw 0.7-phase4b.2"

extern void GPIO_PinMuxFun(int gpio_no, int pin_mux_fun);
extern void GPIO_PinModeConfig(int gpio_no, int pin_mode);

#ifndef MCUFW_UART_CLK
#define MCUFW_UART_CLK 38400000u
#endif
#ifndef MCUFW_UART_BAUD
#define MCUFW_UART_BAUD 115200u
#endif

uint32_t mcufw_boot_marker = 0x4D435546u;
uint32_t mcufw_pinmux_word_txd;
uint32_t mcufw_pinmux_word_rxd;
uint32_t mcufw_uart_divisor;

static void uart0_init(void)
{
	uint32_t div = (MCUFW_UART_CLK + (8u * MCUFW_UART_BAUD)) /
		       (16u * MCUFW_UART_BAUD);

	mcufw_uart_divisor = div;
	CSU_CLK_GATE &= ~(CSU_GATE_UART0 | CSU_GATE_GPIO);
	GPIO_PinMuxFun(MCUFW_PIN_UART0_TXD, 2);
	GPIO_PinMuxFun(MCUFW_PIN_UART0_RXD, 2);
	mcufw_pinmux_word_txd = PINMUX_WORD(MCUFW_PIN_UART0_TXD);
	mcufw_pinmux_word_rxd = PINMUX_WORD(MCUFW_PIN_UART0_RXD);
	UART0_IER_DLM = 0x00;
	UART0_LCR = UART_LCR_DLAB;
	UART0_THR_DLL = div & 0xffu;
	UART0_IER_DLM = (div >> 8) & 0xffu;
	UART0_LCR = UART_LCR_8N1;
	UART0_IIR_FCR = UART_FCR_ENABLE | UART_FCR_RCVR_RST | UART_FCR_XMIT_RST;
	UART0_MCR = 0x00;
}

static void uart_putc(char c)
{
	while (!(UART0_LSR & UART_LSR_THRE))
		;
	UART0_THR_DLL = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s)
{
	while (*s)
		uart_putc(*s++);
}

static void uart_put_hex32(uint32_t v)
{
	uart_puts("0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		uart_putc("0123456789abcdef"[(v >> shift) & 0xf]);
}

static void uart_put_dec(uint32_t v)
{
	char buf[11];
	int i = 0;

	if (!v) {
		uart_putc('0');
		return;
	}
	while (v && i < (int)sizeof(buf) - 1) {
		buf[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i--)
		uart_putc(buf[i]);
}

static void gpio_out_init(int pin)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);
	GPIO_GSDR(bank) = bit;
}

static void gpio_out_set(int pin, int on)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	if (on)
		GPIO_GPSR(bank) = bit;
	else
		GPIO_GPCR(bank) = bit;
}

static int gpio_in_get(int pin)
{
	int bank = pin >> 5;
	return (int)((GPIO_GPLR(bank) >> (pin & 31)) & 1u);
}

static void busy_delay(uint32_t loops)
{
	while (loops--)
		__asm volatile("nop");
}

static void lamp_init(void)
{
	GPIO_PinMuxFun(MCUFW_PIN_GPT0_CH0, MCUFW_PINMUX_FN_GPT0);
	GPIO_PinMuxFun(MCUFW_PIN_GPT0_CH1, MCUFW_PINMUX_FN_GPT0);
	GPIO_PinMuxFun(MCUFW_PIN_GPT0_CH4, MCUFW_PINMUX_FN_GPT0);
	GPIO_PinMuxFun(MCUFW_PIN_GPT0_CH5, MCUFW_PINMUX_FN_GPT0);
	mcufw_gpt0_pwm_init();
}

static void lamp_banner(void)
{
	uart_puts("lamp period ");
	uart_put_dec(MCUFW_LAMP_PERIOD);
	uart_puts(" cmr0");
	for (unsigned i = 0; i < MCUFW_LAMP_CHANNELS; i++) {
		uart_putc(' ');
		uart_put_hex32(mcufw_lamp_cmr0(i));
	}
	uart_puts(" cnt ");
	uart_put_hex32(mcufw_lamp_counter());
	uart_puts("\r\n");
}

#define MCUFW_MIIO_DID 0x05eb8034u

static void uart_put_hex_buf(const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		uart_putc("0123456789abcdef"[p[i] >> 4]);
		uart_putc("0123456789abcdef"[p[i] & 15u]);
	}
}

static void net_selftest(void)
{
	uint32_t fail = mcufw_selftest();
	unsigned n = mcufw_selftest_count();
	unsigned pass = 0;

	for (unsigned i = 0; i < n; i++) {
		int ok = (fail & (1u << i)) == 0u;
		uart_puts("selftest ");
		uart_puts(mcufw_selftest_name(i));
		uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
		if (ok)
			pass++;
	}
	uart_puts("selftest-summary ");
	uart_put_dec(pass);
	uart_putc('/');
	uart_put_dec(n);
	uart_puts(fail == 0u ? " PASS\r\n" : " FAIL\r\n");
}

static void net_miio(void)
{
	mcufw_miio_ctx_t ctx;
	uint8_t pkt[64];
	uint8_t out[64];
	uint8_t digest[16];
	size_t out_len = 0;
	int rc;

	mcufw_miio_ctx_init(&ctx, MCUFW_MIIO_DID);

	uart_puts("miio port ");
	uart_put_dec(MCUFW_MIIO_PORT);
	uart_puts(" did ");
	uart_put_hex32(ctx.did);
	uart_puts(" token ");
	uart_put_hex_buf(ctx.token, MCUFW_MIIO_TOKEN_LEN);
	uart_puts(ctx.have_token ? " paired\r\n" : " unprovisioned\r\n");

	for (unsigned i = 0; i < sizeof(pkt); i++)
		pkt[i] = 0;
	pkt[0] = 0x31;
	pkt[1] = 0x21;
	pkt[2] = (uint8_t)MCUFW_MIIO_HDR_LEN;
	pkt[3] = 0;
	pkt[8] = (uint8_t)(ctx.did);
	pkt[9] = (uint8_t)(ctx.did >> 8);
	pkt[10] = (uint8_t)(ctx.did >> 16);
	pkt[11] = (uint8_t)(ctx.did >> 24);
	pkt[12] = 0x10;

	rc = mcufw_miio_handle(&ctx, pkt, MCUFW_MIIO_HDR_LEN, out, sizeof(out),
			       &out_len);
	uart_puts("miio hello rc ");
	uart_puts(mcufw_miio_strerror(rc));
	uart_puts(" reply-len ");
	uart_put_dec((uint32_t)out_len);

	if (rc == MCUFW_MIIO_OK && out_len == MCUFW_MIIO_HDR_LEN) {
		mcufw_md5_2(out, 16u, ctx.token, MCUFW_MIIO_TOKEN_LEN, digest);
		{
			int ck = 1;
			for (unsigned i = 0; i < MCUFW_MD5_LEN; i++)
				if (digest[i] != out[16 + i])
					ck = 0;
			uart_puts(" checksum ");
			uart_puts(ck ? "ok" : "BAD");
		}
		uart_puts(" reply-checksum ");
		uart_put_hex_buf(out + 16, MCUFW_MIIO_TOKEN_LEN);
	}
	uart_puts("\r\n");
}

/* Phase 3.5 additions: RPC whitelist proof, MQTT topic proof, CF skeleton. */
static void net_rpc_whitelist_proof(void)
{
	const char *blocked[] = {"set_ps", "get_ps", "cfg_lan_ctrl", "ota"};
	const char *allowed[] = {"set_power", "toggle", "set_bright", "get_prop"};

	uart_puts("rpc-whitelist blocked:");
	for (unsigned i = 0; i < 4; i++) {
		uart_puts(" ");
		uart_puts(blocked[i]);
		uart_puts(mcufw_rpc_method_allowed(blocked[i]) ? "=ALLOWED(BAD)" : "=BLOCKED");
	}
	uart_puts("\r\nrpc-whitelist allowed:");
	for (unsigned i = 0; i < 4; i++) {
		uart_puts(" ");
		uart_puts(allowed[i]);
		uart_puts(mcufw_rpc_method_allowed(allowed[i]) ? "=OK" : "=BLOCKED(BAD)");
	}
	uart_puts("\r\n");
}

static void net_mqtt_proof(void)
{
	char buf[256];
	int len;

	len = mcufw_mqtt_discovery_topic(MCUFW_MIIO_DID, buf, sizeof(buf));
	uart_puts("mqtt discovery-topic ");
	for (int i = 0; i < len; i++) uart_putc(buf[i]);
	uart_puts("\r\n");

	len = mcufw_mqtt_state_topic(MCUFW_MIIO_DID, buf, sizeof(buf));
	uart_puts("mqtt state-topic ");
	for (int i = 0; i < len; i++) uart_putc(buf[i]);
	uart_puts("\r\n");

	len = mcufw_mqtt_cmd_topic(MCUFW_MIIO_DID, buf, sizeof(buf));
	uart_puts("mqtt cmd-topic ");
	for (int i = 0; i < len; i++) uart_putc(buf[i]);
	uart_puts("\r\n");
}

static void net_cf_proof(void)
{
	mcufw_cf_params_t params;
	int rc;

	rc = mcufw_cf_parse("[3,1,\"500,1,2700,100\"]", 24, &params);
	uart_puts("cf parse rc ");
	uart_put_dec((uint32_t)(rc >= 0 ? 0 : 1));
	uart_puts(" expr-len ");
	uart_put_dec((uint32_t)params.expr_len);
	uart_puts("\r\n");

	params.action = MCUFW_CF_ACTION_ON;
	rc = mcufw_cf_start(&params);
	uart_puts("cf start rc ");
	uart_put_dec((uint32_t)(rc >= 0 ? 0 : 1));
	uart_puts(" (noop in 3.5)\r\n");
}

/* Phase 3.6: exercise the state model + PWM seam over UART so the live target
 * proves the same path the host test asserts (clamping, toggle, duty word). */
static void net_state_proof(void)
{
	/* Phase 4: die persistente Config wird in den AUTORITATIVEN globalen
	 * Zustand geladen (vor dem ersten Apply).  Die Clamp-/HSV-Diagnose unten
	 * arbeitet danach auf einer lokalen KOPIE, damit sie den Power-on-Zustand
	 * nicht ueberschreibt. */
	mcufw_state_t proof;
	mcufw_state_t *st;
	const mcufw_pwm_stats_t *pwm;

	mcufw_config_init();
	(void)mcufw_config_boot(mcufw_state());
	proof = *mcufw_state();
	st = &proof;
	pwm = mcufw_pwm_stats();
	uart_puts("state init power ");
	uart_put_dec(st->power);
	uart_puts(" bright ");
	uart_put_dec(st->bright);
	uart_puts(" ct ");
	uart_put_dec(st->ct);
	uart_puts(" mode ");
	uart_put_dec(st->color_mode);
	uart_puts("\r\n");

	mcufw_state_set_power(st, 1);
	mcufw_state_set_bright(st, 150);  /* out of range -> clamp to 100 */
	uart_puts("state bright-clamp 150 -> ");
	uart_put_dec(st->bright);
	uart_puts(" pwm-mode ");
	uart_put_dec(pwm->mode);
	uart_puts(" duty ");
	uart_put_dec(mcufw_pwm_duty(st->bright));
	uart_puts("\r\n");

	mcufw_state_set_ct(st, 1000);     /* out of range -> clamp to 1700 */
	uart_puts("state ct-clamp 1000 -> ");
	uart_put_dec(st->ct);
	uart_puts("\r\n");

	mcufw_state_set_hsv(st, 200, 60, 80);
	uart_puts("state hsv 200/60/80 -> pwm-mode ");
	uart_put_dec(pwm->mode);
	uart_puts(" logged [");
	uart_puts(pwm->last_log);
	uart_puts("]\r\n");

	mcufw_state_toggle(st);
	uart_puts("state toggle -> power ");
	uart_put_dec(st->power);
	uart_puts(" (0 = off) pwm-mode ");
	uart_put_dec(pwm->mode);
	uart_puts("\r\n");

	uart_puts("state pwm-calls ");
	uart_put_dec((uint32_t)pwm->set_calls);
	uart_puts(" hsv-calls ");
	uart_put_dec((uint32_t)pwm->hsv_calls);
	uart_puts(" mutations ");
	uart_put_dec(st->mutations);
	uart_puts("\r\n");
}

/* ---- phase 3.8: bounded boot diagnosis + non-blocking service loop ------ */

/* Number of diagnostic ramp steps at boot.  The ramp exists only to prove the
 * four channels are programmable; the permanent path is the state path.
 * 0 disables the ramp completely (build with -DMCUFW_BOOT_SWEEP_ITERS=0). */
#ifndef MCUFW_BOOT_SWEEP_ITERS
#define MCUFW_BOOT_SWEEP_ITERS 3u
#endif

/* THIS IS THE ONLY CALL SITE OF mcufw_lamp_set() IN THE WHOLE IMAGE.
 * It runs exactly MCUFW_BOOT_SWEEP_ITERS times and never returns to it.
 * `make check-owner` fails the build if a second call site shows up. */
static void boot_ramp_sweep(void)
{
	for (uint32_t iter = 1; iter <= (uint32_t)MCUFW_BOOT_SWEEP_ITERS; iter++) {
		uint16_t step = (uint16_t)(iter & 0x3fu);

		mcufw_lamp_set((uint16_t)(step * (MCUFW_LAMP_PERIOD / 64u)));
		uart_puts("mcufw ramp ");
		uart_put_dec(iter);
		uart_puts("/");
		uart_put_dec((uint32_t)MCUFW_BOOT_SWEEP_ITERS);
		uart_puts(" lamp ");
		uart_put_dec(mcufw_lamp_get());
		uart_puts(" cmr0 ");
		uart_put_hex32(mcufw_lamp_cmr0(0));
		uart_puts(" out ");
		uart_put_dec((uint32_t)gpio_in_get(MCUFW_PIN_BOARD_OUT0));
		uart_puts("\r\n");
		/* pacing for the human reading the UART during bring-up; the
		 * service loop afterwards never calls busy_delay again. */
		busy_delay(500000);
	}
}

/* --- I/O the runtime loop is not allowed to do itself -------------------- */
static int rt_board_out(void)
{
	return gpio_in_get(MCUFW_PIN_BOARD_OUT0);
}

static int rt_button(void)
{
	if (MCUFW_BUTTON_GPIO_PIN >= 0)
		return gpio_in_get(MCUFW_BUTTON_GPIO_PIN);
	return -1;
}

static void rt_status(const char *line)
{
	uart_puts(line);
	uart_puts("\r\n");
}

/* Bind UDP 54321 / TCP 55443, then run the service loop for ever.
 * Phase 4b.2: wrapped as FreeRTOS task; no busy_delay, no socket to any peer. */
#ifndef HOST_TEST
static void runtime_task(void *pvParameters)
{
	(void)pvParameters;
#else
static void runtime_loop(void)
{
#endif
	static const mcufw_runtime_io_t io = {
		rt_board_out,
		rt_button,
		rt_status
	};
	int bind_rc;

	mcufw_net_init(NULL, MCUFW_MIIO_DID);
	bind_rc = mcufw_net_lwip_bind();
	mcufw_net_bind_transport(mcufw_net_lwip_transport());

	uart_puts("net bind udp ");
	uart_put_dec((uint32_t)MCUFW_MIIO_PORT);
	uart_puts(" tcp ");
	uart_put_dec((uint32_t)MCUFW_LAN_RPC_PORT);
	uart_puts(" rc ");
	uart_put_dec((uint32_t)(bind_rc == 0 ? 0u : 1u));
	uart_puts(" bound ");
	uart_put_dec((uint32_t)mcufw_net_lwip_bound());
	uart_puts("\r\n");

	mcufw_runtime_init(&io);
	uart_puts("runtime-owner single-writer armed\r\n");

	/* Phase 4b.2: WLAN bringup MUST run after RTOS start (uses FreeRTOS primitives).
	 * Moved here from boot diagnosis per PO-Faktencheck mandate #3. */
	{
		const mcufw_wlan_cfg_t *wcfg = mcufw_wlan_cfg();
		mcufw_wlan_report_t wrep;
		int wrc = mcufw_wlan_bringup(wcfg, &wrep);
		uart_puts("wlan bringup stage ");
		uart_puts(mcufw_wlan_stage_name(wrep.stage));
		uart_puts(" rc ");
		uart_puts(mcufw_wlan_strerror(wrc));
		uart_puts(" state ");
		uart_puts(mcufw_wlan_state_name(mcufw_wlan_get_state()));
		uart_puts("\r\n");
	}

	{
		uint32_t stable_ticks = 0u;
		int boot_confirmed = 0;

		for (;;) {
			mcufw_wlan_state_t ws = mcufw_wlan_get_state();
			int wlan_ready = (ws == MCUFW_WLAN_STATE_STA_CONNECTED ||
			                  ws == MCUFW_WLAN_STATE_AP_FALLBACK);

			mcufw_runtime_tick();
			if (mcufw_rollback_deferred_tick(&stable_ticks, &boot_confirmed,
			                                 wlan_ready,
			                                 MCUFW_ROLLBACK_CONFIRM_TICKS)) {
				uart_puts("rollback: boot confirmed good\r\n");
			}
#ifndef HOST_TEST
			vTaskDelay(pdMS_TO_TICKS(10));
#endif
		}
	}
}

int main(void)
{
	uart0_init();

	/* Phase 4b.1: Early-Boot A/B-Rollback-Check VOR jeglicher RTOS/WLAN/Config-Init. */
	mcufw_rollback_result_t rb = mcufw_rollback_early_boot_check();
	if (rb == MCUFW_RB_ROLLED_BACK) {
		/* Sollte nie hier ankommen (reset ist noreturn), aber falls doch: */
		uart_puts("ROLLBACK: passive slot activated, resetting...\r\n");
		for (;;) ; /* Safety hang; HAL reset sollte bereits erfolgt sein. */
	}
	if (rb == MCUFW_RB_ERROR) {
		uart_puts("ROLLBACK: backend error, continuing with caution");
	}

	uart_puts("\r\n");
	uart_puts(MCUFW_VERSION);
	uart_puts(" -- rtos+lwip+rpc+mqtt+cf+state, no assoc, no cloud\r\n");
	uart_puts("boot-marker ");
	uart_put_hex32(mcufw_boot_marker);
	uart_puts("\r\n");
	uart_puts("uart-clk ");
	uart_put_dec(MCUFW_UART_CLK);
	uart_puts(" baud ");
	uart_put_dec(MCUFW_UART_BAUD);
	uart_puts(" div ");
	uart_put_hex32(mcufw_uart_divisor);
	uart_puts("\r\n");
	uart_puts("pinmux gpio2(txd) ");
	uart_put_hex32(mcufw_pinmux_word_txd);
	uart_puts(" gpio3(rxd) ");
	uart_put_hex32(mcufw_pinmux_word_rxd);
	uart_puts("\r\n");

	GPIO_PinMuxFun(MCUFW_PIN_BOARD_OUT0, MCUFW_PINMUX_FN_GPIO);
	gpio_out_init(MCUFW_PIN_BOARD_OUT0);
	gpio_out_set(MCUFW_PIN_BOARD_OUT0, 1);

	lamp_init();
	lamp_banner();

	net_selftest();
	/* net_wlan() moved to runtime_task (needs RTOS running) */
	net_miio();
	net_rpc_whitelist_proof();
	net_mqtt_proof();
	net_cf_proof();
	net_state_proof();

	uart_puts("gpio-irq grer ");
	uart_put_hex32(GPIO_GRER(0) | GPIO_GRER(1));
	uart_puts(" gfer ");
	uart_put_hex32(GPIO_GFER(0) | GPIO_GFER(1));
	uart_puts(" (0 = keine Taster-IRQ-Quelle scharf)\r\n");
	if (MCUFW_BUTTON_GPIO_PIN >= 0)
		uart_puts("button pin set\r\n");
	else
		uart_puts("button pin none (kein GPIO-Taster im Referenz-Image)\r\n");
	uart_puts("banner-ready\r\n");

	boot_ramp_sweep();

	/* Phase 4: Power-on-Verhalten.  Die Config ist bereits geladen (vor dem
	 * ersten Apply); jetzt erst wird sie auf die Kanaele geschrieben.
	 *   war an  -> wieder an mit letzter Helligkeit/Farbe
	 *   war aus -> alle vier Kanaele dunkel (CMR0 = Periode)
	 * Genau EINE UART-Zeile zum Config-Ladeergebnis, keine Retry-Schleife. */
	{
		const mcufw_config_stats_t *cfg = mcufw_config_stats();

		uart_puts("config backend ");
		uart_puts(mcufw_config_hal_name());
		uart_puts(" load ");
		uart_put_dec((uint32_t)(cfg->last_status == MCUFW_CFG_OK ? 0u : 1u));
		uart_puts(" defaults ");
		uart_put_dec(cfg->load_defaults);
		uart_puts(" power ");
		uart_put_dec(mcufw_state()->power);
		uart_puts(" bright ");
		uart_put_dec(mcufw_state()->bright);
		uart_puts("\r\n");
		if (cfg->last_log[0]) {
			uart_puts(cfg->last_log);
			uart_puts("\r\n");
		}
	}

	/* Hand the lamp over to the state path: the ramp wrote CMR0 behind the
	 * PWM backend's back, so forget the programmed command and re-apply the
	 * authoritative state once.  From here on mcufw_pwm is the only writer. */
	mcufw_pwm_invalidate();
	mcufw_state_apply(mcufw_state());
	uart_puts("owner mcufw_pwm lamp ");
	uart_put_dec(mcufw_pwm_stats()->ch_level[3]);
	uart_put_dec(mcufw_pwm_stats()->ch_level[0]);
	uart_puts(" cmr0 ");
	uart_put_hex32(mcufw_pwm_cmr0(0));
	uart_puts("\r\n");

	/* Ab hier ist der State-Pfad der einzige Schreiber; ein Tick speichert
	 * nur bei einer NEUEN Mutation, ein Idle-Tick schreibt 0 Config-Bytes. */
#ifndef HOST_TEST
	/* Create the runtime task and start the FreeRTOS scheduler (never returns). */
	xTaskCreate(runtime_task, "runtime", 2048, NULL, 2, NULL);
	mcufw_rtos_start();
#else
	/* Host-test: run the loop directly (no scheduler). */
	runtime_loop();
#endif
}
