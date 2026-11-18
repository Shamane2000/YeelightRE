/* main.c — mcufw phase 3.2: real Yeelight hardware I/O path.
 *
 * Cloud-free: no WLAN, no miIO/RPC, no OTA, no RTOS.
 *
 * What is new in 3.2 (all pin numbers proven against color1_0061.elf,
 * see docs/08_pinmux_uart.md):
 *   - pinmux is programmed through the VENDOR driver GPIO_PinMuxFun()
 *     out of libwmsdk.a (mw300_pinmux.o) instead of a hand-rolled store
 *   - GPIO drive path uses GSDR/GCDR (data) + GPSR/GPCR (direction), the
 *     semantics the reference binary actually uses
 *   - UART clock corrected from the assumed 40 MHz to the 38.4 MHz main XTAL
 *     (mw300_clock.h CLK_MAINXTAL_FREQUENCY)
 *   - banner prints the configured pinmux words (readback) so a live test can
 *     be checked without a debugger
 */
#include "mcufw_regs.h"

#define MCUFW_VERSION "mcufw 0.2-phase3.2"

/* --- vendor driver entry points (libwmsdk.a:mw300_pinmux.o) --------------- *
 * Declared locally instead of including mw300_pinmux.h: the vendor header drags
 * in the whole SDK include chain (mw300.h, interrupt tables, ...) which the
 * freestanding mcufw does not need.  The enums are int-sized, so the ABI
 * matches the vendor implementation exactly -- verified by linking and by
 * disassembling the resulting image (nm shows GPIO_PinMuxFun in mcufw.elf). */
extern void GPIO_PinMuxFun(int gpio_no, int pin_mux_fun);
extern void GPIO_PinModeConfig(int gpio_no, int pin_mode);

/* --- clock / baud -------------------------------------------------------- *
 * Phase 3.1 assumed a 40 MHz UART clock.  The vendor SDK pins the main crystal
 * at 38.4 MHz (mw300_clock.h: CLK_MAINXTAL_FREQUENCY = 38400000).  The UART
 * clock is either the slow XTAL or the fast PLL branch selected by boot2
 * (CLK_SetUARTClkSrc / CLK_UARTDividerSet); mcufw does not reprogram the clock
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

/* --- GPIO helpers (semantics proven from color1_0061.elf) ---------------- */
static void gpio_out_init(int pin)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	GPIO_GPSR(bank) = bit;               /* GPSR: set direction = output   */
	GPIO_GCDR(bank) = bit;               /* GCDR: drive data low           */
}

static void gpio_out_set(int pin, int on)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	if (on)
		GPIO_GSDR(bank) = bit;       /* GSDR: drive data high          */
	else
		GPIO_GCDR(bank) = bit;       /* GCDR: drive data low           */
}

static int gpio_in_get(int pin)
{
	int bank = pin >> 5;

	return (GPIO_GPLR(bank) >> (pin & 31)) & 1u;
}

static void busy_delay(uint32_t loops)
{
	while (loops--)
		__asm volatile("nop");
}

int main(void)
{
	uart0_init();

	uart_puts("\r\n");
	uart_puts(MCUFW_VERSION);
	uart_puts(" -- no WLAN, no miIO, no cloud\r\n");
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
	uart_puts("pin gpio44 level ");
	uart_put_dec((uint32_t)gpio_in_get(MCUFW_PIN_BOARD_OUT0));
	uart_puts("\r\n");
	uart_puts("banner-ready\r\n");

	/* Board output proven driven high by color1_0061 (code 0x1f03a05c).
	 * Used as a heartbeat until the GPT0 lamp PWM path lands (phase 3.3). */
	GPIO_PinMuxFun(MCUFW_PIN_BOARD_OUT0, MCUFW_PINMUX_FN_GPIO);
	gpio_out_init(MCUFW_PIN_BOARD_OUT0);

	for (unsigned iter = 1;; iter++) {
		gpio_out_set(MCUFW_PIN_BOARD_OUT0, 1);
		busy_delay(2000000);
		gpio_out_set(MCUFW_PIN_BOARD_OUT0, 0);
		busy_delay(2000000);

		uart_puts("mcufw loop ");
		uart_put_hex32(iter);
		uart_puts(" gpio44=");
		uart_put_dec((uint32_t)gpio_in_get(MCUFW_PIN_BOARD_OUT0));
		uart_puts("\r\n");
	}
}
