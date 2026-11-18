/* main.c — mcufw phase 3.6: lamp state model + PWM backend abstraction.
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

#define MCUFW_VERSION "mcufw 0.6-phase3.6"

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

static void net_wlan(void)
{
	const mcufw_wlan_cfg_t *cfg = mcufw_wlan_cfg();
	mcufw_wlan_anchor_t anchors[8];
	mcufw_wlan_report_t rep;
	unsigned count = mcufw_wlan_link_proof(anchors, 8);
	int rc = mcufw_wlan_bringup(cfg, &rep);

	uart_puts("wlan cfg provisioned ");
	uart_put_dec(cfg->provisioned);
	uart_puts(" muid ");
	uart_put_hex32(cfg->muid);
	uart_puts("\r\n");

	for (unsigned i = 0; i < count && i < 8u; i++) {
		uart_puts("wlan sym ");
		uart_puts(anchors[i].name);
		uart_puts(" @");
		uart_put_hex32((uint32_t)(uintptr_t)anchors[i].addr);
		uart_puts(" ");
		uart_puts(anchors[i].member);
		uart_puts("\r\n");
	}

	uart_puts("wlan bringup stage ");
	uart_puts(mcufw_wlan_stage_name(rep.stage));
	uart_puts(" rc ");
	uart_puts(mcufw_wlan_strerror(rc));
	uart_puts(" (assoc = phase 5)\r\n");
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
	mcufw_state_t *st = mcufw_state();
	const mcufw_pwm_stats_t *pwm;

	mcufw_state_init(st);
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

int main(void)
{
	uart0_init();

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
	net_wlan();
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

	for (unsigned iter = 1;; iter++) {
		uint16_t step = (uint16_t)(iter & 0x3fu);

		mcufw_lamp_set((uint16_t)(step * (MCUFW_LAMP_PERIOD / 64u)));
		busy_delay(2000000);

		if ((iter & 0x1fu) == 0u) {
			uart_puts("mcufw loop ");
			uart_put_hex32(iter);
			uart_puts(" lamp ");
			uart_put_dec(mcufw_lamp_get());
			uart_puts(" cmr0 ");
			uart_put_hex32(mcufw_lamp_cmr0(0));
			uart_puts(" out ");
			uart_put_dec((uint32_t)gpio_in_get(MCUFW_PIN_BOARD_OUT0));
			uart_puts("\r\n");
		}
	}
}
