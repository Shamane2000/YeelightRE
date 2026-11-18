/* main.c — minimal mcufw: SoC bring-up, UART0 console, LED blink.
 *
 * Deliberately cloud-free: no WLAN, no miIO/RPC, no OTA, no RTOS.
 * Only the register blocks documented in mcufw_regs.h are touched.
 */
#include "mcufw_regs.h"

#define MCUFW_VERSION "mcufw 0.1-phase3.1"

/* Initialised global -> non-empty .data section, so the container really carries
 * the third load segment at 0x20000040 (SRAM1), exactly like the vendor image
 * color1_0061. Without any initialised data the linker drops .data and the
 * container would only have two segments. */
uint32_t mcufw_boot_marker = 0x4D435546u;   /* "MCUF" */

/* UART fast clock as left behind by boot2 -- NOT programmed here. The value is
 * the APB0/UART fast clock assumption documented in
 * docs/07_sdk_build_bringup.md; a wrong value only shifts the baud rate. */
#ifndef MCUFW_UART_CLK
#define MCUFW_UART_CLK 40000000u
#endif
#ifndef MCUFW_UART_BAUD
#define MCUFW_UART_BAUD 115200u
#endif

static void uart0_init(void)
{
	uint32_t div = (MCUFW_UART_CLK + (8u * MCUFW_UART_BAUD)) /
		       (16u * MCUFW_UART_BAUD);

	/* Ungate UART0 + GPIO in the CSU (enable = clear gate bit). */
	CSU_CLK_GATE &= ~(CSU_GATE_UART0 | CSU_GATE_GPIO);

	/* Route UART0 onto GPIO2/GPIO3 (function 2 on both pins). */
	PINMUX_PIN(MCUFW_PIN_UART0_TXD) = PINMUX_FSEL(2);
	PINMUX_PIN(MCUFW_PIN_UART0_RXD) = PINMUX_FSEL(2);

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

static void led_init(int pin)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	PINMUX_PIN(pin) = PINMUX_FSEL(0);    /* pin as plain GPIO */
	GPIO_GPDR(bank) |= bit;              /* output            */
	GPIO_GPCR(bank) = bit;               /* start low         */
}

static void led_set(int pin, int on)
{
	int bank = pin >> 5;
	uint32_t bit = 1u << (pin & 31);

	if (on)
		GPIO_GPSR(bank) = bit;
	else
		GPIO_GPCR(bank) = bit;
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
	uart_puts("banner-ready\r\n");

	led_init(MCUFW_LED_PIN);

	for (unsigned iter = 1;; iter++) {
		led_set(MCUFW_LED_PIN, 1);
		busy_delay(2000000);
		led_set(MCUFW_LED_PIN, 0);
		busy_delay(2000000);

		uart_puts("mcufw loop ");
		uart_put_hex32(iter);
		uart_puts("\r\n");
	}
}
