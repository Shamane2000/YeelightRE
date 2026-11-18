/* mcufw_regs.h — 88MW300/88MW302 register map for the standalone mcufw.
 *
 * PHASE 3.2: pinmux + GPIO offsets are now *derived from the reference binary*
 * color1_0061.elf and cross-checked against the vendor register headers.
 *
 * Provenance (every value below is traceable, no guessing):
 *   - Peripheral base addresses: sdk/src/incl/sdk/drivers/mw300/mw300.h
 *       (PERI_APB0_BASE 0x46000000 -> UART0 0x46040000, GPIO 0x46060000,
 *        PERI_APB1_BASE 0x48000000 -> PINMUX 0x48010000)
 *   - PINMUX word layout: regs/pinmux_reg.h (union BF)
 *       FSEL_XR [2:0], DI_EN [3], PIO_PULLDN_R [13], PIO_PULLUP_R [14],
 *       PIO_PULL_SEL_R [15];  one word per pin: PINMUX_BASE + 4*pin
 *     Confirmed by disassembly of GPIO_PinMuxFun (in libwmsdk.a) at
 *     code 0x1f036584 of color1_0061.elf: index = base + pin*4, bits[2:0]
 *     = function, bit15 cleared whenever function != 0 (all non-GPIO
 *     functions clear PIO_PULL_SEL_R), plus a bit[3] DI_EN variant.
 *   - GPIO register blocks: regs/gpio_reg.h, 3 banks x 32 pins, one word per
 *     bank (0x00 GPLR, 0x0C GPDR, 0x18 GPSR, 0x24 GPCR, 0x30 GRER, ...,
 *     0x54 GSDR, 0x60 GCDR, ...).  Confirmed by disassembly of the GPIO
 *     accessors in color1_0061.elf:
 *       0x1f03669c gpio_out(pin,1) -> 0x46060054 + bank*4   (GSDR, set data)
 *       0x1f03669c gpio_out(pin,0) -> 0x46060060 + bank*4   (GCDR, clr data)
 *       0x1f0366c8 dir=1          -> 0x46060018 + bank*4   (GPSR, output)
 *       0x1f0366c8 dir=0          -> 0x46060024 + bank*4   (GPCR, input)
 *       0x1f0366f2 gpio_read      -> 0x46060000 + bank*4   (GPLR)
 *     => GPSR/GPCR configure the DIRECTION (set/clear GPDR), GSDR/GCDR drive
 *        the OUTPUT DATA.  Phase 3.1 wrote data through GPSR/GPCR, which only
 *        toggled the pin direction and never drove a level.
 *   - UART register offsets: regs/uart_reg.h (16550-compatible block)
 *   - UART clock: mw300_clock.h  CLK_MAINXTAL_FREQUENCY = 38400000 (38.4 MHz).
 *     Phase 3.1 assumed 40 MHz -- corrected here, see docs/08_pinmux_uart.md.
 *
 * NOT included on purpose: WLAN/miIO/cloud registers, flash controller setup,
 * PMU/clock-tree programming, GPT (lamp PWM) and interrupt routing.  mcufw runs
 * on the clock tree that boot2 leaves behind (see sdk/README.md).
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

/* --- pin mux (regs/pinmux_reg.h) ----------------------------------------- */
#define PINMUX_WORD(pin)    REG32(PINMUX_BASE + 4u * (pin))
#define PINMUX_FSEL_MASK    0x7u
#define PINMUX_DI_EN        0x8u          /* bit3  input enable             */
#define PINMUX_PULLDN       0x2000u       /* bit13 pull-down resistor        */
#define PINMUX_PULLUP       0x4000u       /* bit14 pull-up resistor          */
#define PINMUX_PULL_SEL     0x8000u       /* bit15 pull selected / fn valid  */

/* --- pin assignment, derived from color1_0061.elf (docs/08) -------------- */
/* UART0: code 0x1f0117d2 (fsel(2,2) / fsel(3,2)), 8-bit per-uart dispatcher. */
#define MCUFW_PIN_UART0_TXD     2         /* GPIO2  FSEL 2 = UART0_TXD       */
#define MCUFW_PIN_UART0_RXD     3         /* GPIO3  FSEL 2 = UART0_RXD       */
/* UART2 alternative found in the same dispatcher (code 0x1f0117f0/fc).     */
#define MCUFW_PIN_UART2_TXD     48        /* GPIO48 FSEL 2 = UART2_TXD       */
#define MCUFW_PIN_UART2_RXD     49        /* GPIO49 FSEL 2 = UART2_RXD       */
/* Lamp PWM: code 0x1f00e27c..0x1f00e2d8 -> fsel(0|1|4|5, 1) + dir=out.
 * FSEL 1 on these pins is GPT0_CH0/CH1/CH4/CH5 (mw300_pinmux.h) -- the lamp is
 * driven by GPT0 PWM, there is no single "LED" GPIO.                       */
#define MCUFW_PIN_GPT0_CH0      0
#define MCUFW_PIN_GPT0_CH1      1
#define MCUFW_PIN_GPT0_CH4      4
#define MCUFW_PIN_GPT0_CH5      5
#define MCUFW_PINMUX_FN_GPT0    1
/* Discrete board outputs: code 0x1f03a04c..0x1f03a08c (fsel(44|45,0), dir,
 * driven high; a fsel(44|45,7) variant exists for the other board revision). */
#define MCUFW_PIN_BOARD_OUT0    44
#define MCUFW_PIN_BOARD_OUT1    45
#define MCUFW_PINMUX_FN_GPIO    0

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

/* --- GPIO (3 banks x 32 pins, one word per bank; regs/gpio_reg.h) -------- */
#define GPIO_GPLR(bank)     REG32(GPIO_BASE + 0x00u + 4u * (bank))  /* in data */
#define GPIO_GPDR(bank)     REG32(GPIO_BASE + 0x0cu + 4u * (bank))  /* dir(ro) */
#define GPIO_GPSR(bank)     REG32(GPIO_BASE + 0x18u + 4u * (bank))  /* dir set */
#define GPIO_GPCR(bank)     REG32(GPIO_BASE + 0x24u + 4u * (bank))  /* dir clr */
#define GPIO_GSDR(bank)     REG32(GPIO_BASE + 0x54u + 4u * (bank))  /* data 1  */
#define GPIO_GCDR(bank)     REG32(GPIO_BASE + 0x60u + 4u * (bank))  /* data 0  */

#endif /* MCUFW_REGS_H */
