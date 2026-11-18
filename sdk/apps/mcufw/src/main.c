/* main.c — mcufw phase 3.3: lamp PWM + corrected GPIO data/direction path.
 *
 * Cloud-free: no WLAN, no miIO/RPC, no OTA, no RTOS.
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

#define MCUFW_VERSION "mcufw 0.3-phase3.3"

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

	/* Board output proven driven high by color1_0061 (code 0x1f03a05c). */
	GPIO_PinMuxFun(MCUFW_PIN_BOARD_OUT0, MCUFW_PINMUX_FN_GPIO);
	gpio_out_init(MCUFW_PIN_BOARD_OUT0);
	gpio_out_set(MCUFW_PIN_BOARD_OUT0, 1);

	/* Lamp path: GPT0 PWM on the four real channels. */
	lamp_init();
	lamp_banner();

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
