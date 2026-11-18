/* mcufw_regs.h — minimal 88MW300/88MW302 register map for the standalone mcufw.
 *
 * Provenance (every value below is traceable, no guessing):
 *   - Peripheral base addresses + widths:
 *       EZ-Connect-Lite sdk/src/incl/sdk/drivers/mw300/mw300.h
 *       (PERI_APB0_BASE 0x46000000 -> UART0 0x46040000, GPIO 0x46060000,
 *        PERI_APB1_BASE 0x48000000 -> PINMUX 0x48010000)
 *   - UART register offsets: regs/uart_reg.h (16550-compatible block)
 *   - GPIO register layout: regs/gpio_reg.h (GPLR/GPDR/GPSR/GPCR, 3 banks)
 *   - Pin mux word layout: regs/pinmux_reg.h (FSEL_XR bits [2:0], one word per pin)
 *   - Pin function numbers: mw300_pinmux.h
 *       GPIO2_UART0_TXD = PINMUX_FUNCTION_2, GPIO3_UART0_RXD = PINMUX_FUNCTION_2
 *   - Clock gate unit (CSU) base + gate bits: disassembly of CLK_ModuleClkEnable()
 *     in sdk/libs/libwmsdk.a (mw300_clock.o; literal pool: 0x480a0000, gate
 *     register +0x7c, enable = clear bit; CLK_UART0 = bit5, CLK_GPIO = bit4;
 *     enum order from mw300_clock.h CLK_Module_Type).
 *
 * NOT included on purpose: WLAN/miIO/cloud registers, flash controller setup,
 * PMU/clock-tree programming. mcufw runs on the clock tree that boot2 leaves
 * behind (see sdk/README.md, "Offene Punkte").
 */
#ifndef MCUFW_REGS_H
#define MCUFW_REGS_H

#include <stdint.h>

/* --- memory map (mw300-xip.ld / color1_0061 evidence) --------------------- */
#define MCUFW_STACK_TOP     0x20020000u  /* end of SRAM1 (128K @0x20000000)   */
#define MCUFW_INIT_BASE     0x00100000u  /* SRAM0 (384K @0x00100000)          */
#define MCUFW_DATA_BASE     0x20000040u  /* .data load address, color1_0061   */
#define MCUFW_FLASHC_WINDOW 0x1f000000u  /* flash controller memory window    */
#define SCB_VTOR            0xe000ed08u

/* --- peripheral bases ---------------------------------------------------- */
#define PINMUX_BASE         0x48010000u
#define CSU_BASE            0x480a0000u
#define UART0_BASE          0x46040000u
#define GPIO_BASE           0x46060000u

#define REG32(addr)         (*(volatile uint32_t *)(addr))

/* --- CSU clock gating (enable = clear gate bit) -------------------------- */
#define CSU_CLK_GATE        REG32(CSU_BASE + 0x7cu)
#define CSU_GATE_GPIO       (1u << 4)
#define CSU_GATE_UART0      (1u << 5)

/* --- pin mux ------------------------------------------------------------- */
#define PINMUX_PIN(n)       REG32(PINMUX_BASE + 4u * (n))
#define PINMUX_FSEL(f)      ((f) & 0x7u)
#define MCUFW_PIN_UART0_TXD 2            /* GPIO2, FSEL 2 = UART0_TXD */
#define MCUFW_PIN_UART0_RXD 3            /* GPIO3, FSEL 2 = UART0_RXD */

/* --- UART0 (16550-compatible) -------------------------------------------- */
#define UART0_THR_DLL       REG32(UART0_BASE + 0x00u)
#define UART0_IER_DLM       REG32(UART0_BASE + 0x04u)
#define UART0_IIR_FCR       REG32(UART0_BASE + 0x08u)
#define UART0_LCR           REG32(UART0_BASE + 0x0cu)
#define UART0_MCR           REG32(UART0_BASE + 0x10u)
#define UART0_LSR           REG32(UART0_BASE + 0x14u)
#define UART0_SCR           REG32(UART0_BASE + 0x1cu)

#define UART_LCR_DLAB       0x80u
#define UART_LCR_8N1        0x03u
#define UART_FCR_ENABLE     0x01u
#define UART_FCR_RCVR_RST   0x02u
#define UART_FCR_XMIT_RST   0x04u
#define UART_LSR_THRE       (1u << 5)
#define UART_LSR_TEMT       (1u << 6)

/* --- GPIO (3 banks of 32 pins) ------------------------------------------- */
#define GPIO_GPLR(bank)     REG32(GPIO_BASE + 0x00u + 4u * (bank))
#define GPIO_GPDR(bank)     REG32(GPIO_BASE + 0x0cu + 4u * (bank))
#define GPIO_GPSR(bank)     REG32(GPIO_BASE + 0x18u + 4u * (bank))
#define GPIO_GPCR(bank)     REG32(GPIO_BASE + 0x24u + 4u * (bank))

#define MCUFW_LED_PIN       40           /* mw302_rd board_led_1(); Yeelight
                                          * pin mapping not yet confirmed -- see
                                          * docs/07_sdk_build_bringup.md       */

#endif /* MCUFW_REGS_H */
