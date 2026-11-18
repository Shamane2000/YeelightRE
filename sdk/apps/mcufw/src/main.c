/* main.c — mcufw phase 3.4: WLAN link boundary + miIO UDP channel (54321).
 *
 * New in 3.4:
 *   - mcufw_wlan.c: libwmsdk.a(mlan_init.o)+(mdev_startup.o) linked and the
 *     bring-up state machine (cfg -> link -> mlan -> assoc) runs up to the
 *     RTOS gate.  No association yet (phase 5) -- and the banner says so.
 *   - mcufw_miio.c: miIO-compatible UDP channel on port 54321 (32-byte
 *     header, hello handshake incl. token hand-out, MD5 checksum,
 *     AES-128-CBC payload, method dispatch).  `miIO.ota` is the only OTA
 *     channel (docs/06).  No LAN-JSON-RPC, no MQTT (that is 3.5).
 *   - mcufw_crypto.c: own MD5 + AES-128-CBC so the miIO layer does not have
 *     to guess the layout of the vendor crypto contexts.
 *   - mcufw_shim.c: link shims for the FreeRTOS/lwIP surface that the mlan
 *     object graph references but 3.4 does not own; all inert by design.
 *
 * Still cloud-free: no association, no cloud MQTT, no OTA download.
 *
 * 3.3 (kept): lamp PWM on GPT0 CH0/1/4/5 and the corrected GPIO
 * data/direction path (GPSR/GPCR = data, GSDR/GCDR = direction).
 *
 * New in 3.3 (all offsets proven against color1_0061.elf, docs/09):
 *   - GPIO data/direction semantics of phase 3.2 CORRECTED: GPSR/GPCR carry
 *     the DATA, GSDR/GCDR set/clear the DIRECTION (mw300_gpio.h API names +
 *     regs/gpio_reg.h offset comments).  Phase 3.2 drove the board output
 *     through the direction registers, i.e. it left the pin an input.
 *   - GPT0 PWM lamp driver (mcufw_gpt.c): four channels CH0/CH1/CH4/CH5 on
 *     GPIO0/1/4/5/FSEL 1, period 0xC350, inverted duty -- the real Yeelight
 *     lamp path instead of a GPIO heartbeat.
 *   - taster: no GPIO button exists in this firmware (proof in mcufw_regs.h);
 *     the banner reports that the GPIO IRQ path carries no armed pin.
 */
#include "mcufw_regs.h"
#include "mcufw_gpt.h"
#include "mcufw_wlan.h"
#include "mcufw_miio.h"
#include "mcufw_crypto.h"
#include "mcufw_selftest.h"

#define MCUFW_VERSION "mcufw 0.4-phase3.4"

/* --- vendor driver entry points (libwmsdk.a:mw300_pinmux.o) --------------- *
 * Declared locally instead of including mw300_pinmux.h: the vendor header drags
 * in the whole SDK include chain (mw300.h, interrupt tables, ...) which the
 * freestanding mcufw does not need.  The enums are int-sized, so the ABI
 * matches the vendor implementation exactly -- verified by linking and by
 * disassembling the resulting image (nm shows GPIO_PinMuxFun in mcufw.elf). */
extern void GPIO_PinMuxFun(int gpio_no, int pin_mux_fun);
extern void GPIO_PinModeConfig(int gpio_no, int pin_mode);

/* --- clock / baud -------------------------------------------------------- *
 * The vendor SDK pins the main crystal at 38.4 MHz (mw300_clock.h:
 * CLK_MAINXTAL_FREQUENCY = 38400000).  mcufw does not reprogram the clock
 * tree, so the value is a parameter and the banner prints what was assumed. */
#ifndef MCUFW_UART_CLK
#define MCUFW_UART_CLK 38400000u
#endif
#ifndef MCUFW_UART_BAUD
#define MCUFW_UART_BAUD 115200u
#endif

/* Initialised global -> non-empty .data section, so the container really carries
 * the third load segment at 0x20000040 (SRAM1), exactly like color1_0061. */
uint32_t mcufw_boot_marker = 0x4D435546u;   /* "MCUF" */
uint32_t mcufw_pinmux_word_txd;             /* readback, filled in pinmux_init */
uint32_t mcufw_pinmux_word_rxd;
uint32_t mcufw_uart_divisor;

static void uart0_init(void)
{
	uint32_t div = (MCUFW_UART_CLK + (8u * MCUFW_UART_BAUD)) /
		       (16u * MCUFW_UART_BAUD);

	mcufw_uart_divisor = div;

	/* Ungate UART0 + GPIO in the CSU (enable = clear gate bit). */
	CSU_CLK_GATE &= ~(CSU_GATE_UART0 | CSU_GATE_GPIO);

	/* Route UART0 onto GPIO2/GPIO3 -- function 2 on both pins, exactly as the
	 * reference dispatcher does at code 0x1f0117d2. */
	GPIO_PinMuxFun(MCUFW_PIN_UART0_TXD, 2);
	GPIO_PinMuxFun(MCUFW_PIN_UART0_RXD, 2);
	mcufw_pinmux_word_txd = PINMUX_WORD(MCUFW_PIN_UART0_TXD);
	mcufw_pinmux_word_rxd = PINMUX_WORD(MCUFW_PIN_UART0_RXD);

	UART0_IER_DLM = 0x00;                /* no interrupts          */
	UART0_LCR = UART_LCR_DLAB;           /* divisor latch access   */
	UART0_THR_DLL = div & 0xffu;
	UART0_IER_DLM = (div >> 8) & 0xffu;
	UART0_LCR = UART_LCR_8N1;            /* 8N1, DLAB off          */
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

/* --- GPIO helpers (semantics proven from regs/gpio_reg.h + mw300_gpio.h) - */
static void gpio_out_init(int pin)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	GPIO_GSDR(bank) = bit;               /* GSDR: set DIRECTION = output   */
}

static void gpio_out_set(int pin, int on)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	if (on)
		GPIO_GPSR(bank) = bit;       /* GPSR: DATA = 1                 */
	else
		GPIO_GPCR(bank) = bit;       /* GPCR: DATA = 0                 */
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

/* --- lamp ----------------------------------------------------------------- */
static void lamp_init(void)
{
	/* Pin mux first: FSEL 1 on GPIO0/1/4/5 = GPT0_CH0/CH1/CH4/CH5
	 * (reference code 0x1f00e286..0x1f00e2d8). */
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

/* --- network layer, phase 3.4 -------------------------------------------- */
#define MCUFW_MIIO_DID 0x05eb8034u   /* did of the reference carrier (.115) */

static void uart_put_hex_buf(const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		uart_putc("0123456789abcdef"[p[i] >> 4]);
		uart_putc("0123456789abcdef"[p[i] & 15u]);
	}
}

/* Known-answer tests: RFC 1321 (MD5), FIPS-197 (AES block), SP 800-38A
 * (AES-CBC) and the miIO hello/OTA round trips.  Same code as the host test. */
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

/* In-image round trip of the miIO hello: the handler answers its own request,
 * so header layout, token hand-out and MD5 checksum are exercised on the real
 * target.  The encrypted path (miIO.ota) is covered by the selftest above. */
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

	/* 32-byte hello, all-zero body (shape observed towards the carrier). */
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
	pkt[12] = 0x10;                 /* stamp; no RTC in 3.4 -> fixed test */

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

int main(void)
{
	uart0_init();

	uart_puts("\r\n");
	uart_puts(MCUFW_VERSION);
	uart_puts(" -- wlan link + miio channel, no assoc, no cloud\r\n");
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

	/* Board output proven driven high by color1_0061 (code 0x1f03a05c). */
	GPIO_PinMuxFun(MCUFW_PIN_BOARD_OUT0, MCUFW_PINMUX_FN_GPIO);
	gpio_out_init(MCUFW_PIN_BOARD_OUT0);
	gpio_out_set(MCUFW_PIN_BOARD_OUT0, 1);

	/* Lamp path: GPT0 PWM on the four real channels. */
	lamp_init();
	lamp_banner();

	/* Network layer (phase 3.4): known-answer selftest, WLAN link boundary,
	 * miIO channel.  All three run before the sweep so a serial capture is
	 * enough to judge the build. */
	net_selftest();
	net_wlan();
	net_miio();

	/* Taster: no GPIO pin is armed in this firmware (see mcufw_regs.h).
	 * Report the interrupt-enable state of the edge detectors instead of
	 * pretending a button exists: GRER/GFER == 0 for all banks means the
	 * GPIO IRQ path is electrically silent. */
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

	/* Breathing sweep over the four lamp channels.  Level 0..period in 64
	 * steps, mirrored, so every channel is exercised in both directions. */
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
