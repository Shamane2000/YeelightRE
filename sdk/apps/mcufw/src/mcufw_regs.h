/* mcufw_regs.h — 88MW300/88MW302 register map for the standalone mcufw.
 *
 * PHASE 3.3: GPIO data/direction semantics of phase 3.2 CORRECTED, GPT0
 * (lamp PWM), the interrupt level and the taster evidence added.
 *
 * Provenance (every value below is traceable, no guessing):
 *   - Peripheral base addresses: sdk/src/incl/sdk/drivers/mw300/mw300.h
 *       PERI_APB0_BASE 0x46000000 -> UART0 0x46040000, GPIO 0x46060000,
 *                                    GPT0 0x46070000, GPT1 0x46080000
 *       PERI_APB1_BASE 0x48000000 -> PINMUX 0x48010000
 *     GPT0 base double-checked against color1_0061.elf: the only literal
 *     0x46070000 in the image sits at 0x1f048ffc (gpt driver pointer table).
 *   - PINMUX word layout: regs/pinmux_reg.h (union BF)
 *       FSEL_XR [2:0], DI_EN [3], PIO_PULLDN_R [13], PIO_PULLUP_R [14],
 *       PIO_PULL_SEL_R [15];  one word per pin: PINMUX_BASE + 4*pin
 *     Confirmed by disassembly of GPIO_PinMuxFun (in libwmsdk.a) at
 *     code 0x1f036584 of color1_0061.elf.
 *   - GPIO register blocks: regs/gpio_reg.h, 3 banks x 32 pins, one word per
 *     bank.  The offset comments in that header are authoritative:
 *       0x00-0x08 GPLR  pin level          (r/o)
 *       0x0c-0x14 GPDR  pin DIRECTION      (r/w)   <-- direction, not data
 *       0x18-0x20 GPSR  pin set   (DATA 1) (w/o)
 *       0x24-0x2c GPCR  pin clear (DATA 0) (w/o)
 *       0x30-0x38 GRER  rising edge enable
 *       0x3c-0x44 GFER  falling edge enable
 *       0x48-0x50 GEDR  edge detect status (w1clr)
 *       0x54-0x5c GSDR  bitwise SET DIRECTION   (output)
 *       0x60-0x68 GCDR  bitwise CLEAR DIRECTION (input)
 *       0x6c-0x74 GSRER set rising edge enable
 *       0x78-0x80 GCRER clear rising edge enable
 *       0x84-0x8c GSFER set falling edge enable
 *       0x90-0x98 GCFER clear falling edge enable
 *     Cross-check from color1_0061.elf accessors:
 *       0x1f03669c(pin,1) -> 0x46060054 (GSDR) : pin mode/direction helper
 *       0x1f03669c(pin,0) -> 0x46060060 (GCDR)
 *       0x1f0366c8(pin,1) -> 0x46060018 (GPSR) : "drive high"
 *       0x1f0366c8(pin,0) -> 0x46060024 (GPCR) : "drive low"
 *       0x1f0366f2(pin)   -> 0x46060000 (GPLR) : level read
 *     => PHASE 3.2 HAD THIS SWAPPED (it drove data through GPSR/GPCR and set
 *        the direction through GSDR/GCDR).  The vendor API names in
 *        mw300_gpio.h settle it: GPIO_SetPinDir() touches GSDR/GCDR,
 *        GPIO_WritePinOutput() touches GPSR/GPCR.
 *   - GPT register layout: regs/gpt_reg.h (one 0x200 header + 6 channel
 *     blocks of 0x30 bytes starting at 0x200).
 *   - UART register offsets: regs/uart_reg.h (16550-compatible block)
 *   - UART clock: mw300_clock.h  CLK_MAINXTAL_FREQUENCY = 38400000 (38.4 MHz).
 *
 * NOT included on purpose: WLAN/miIO/cloud registers, flash controller setup,
 * PMU/clock-tree programming.  mcufw runs on the clock tree boot2 leaves
 * behind (see sdk/README.md) -- only the GPT0 clock control it owns itself is
 * programmed, in mcufw_gpt.c.
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
#define SCB_ISER            0xe000e100u  /* NVIC set-enable, 1 bit per IRQ    */
#define SCB_ICER            0xe000e180u

/* --- peripheral bases ---------------------------------------------------- */
#define PINMUX_BASE         0x48010000u
#define CSU_BASE            0x480a0000u
#define UART0_BASE          0x46040000u
#define GPIO_BASE           0x46060000u
#define GPT0_BASE           0x46070000u

#define REG32(addr)         (*(volatile uint32_t *)(addr))

/* --- CSU clock gating (enable = clear gate bit) -------------------------- */
#define CSU_CLK_GATE        REG32(CSU_BASE + 0x7cu)
#define CSU_GATE_GPIO       (1u << 4)
#define CSU_GATE_UART0      (1u << 5)
#define CSU_GATE_GPT0       (1u << 6)   /* position after GPIO(4)/UART0(5);
					 * no CSU gate table in the vendor
					 * headers -> assumption, see docs/09 */

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
/* Lamp PWM: code 0x1f00e27c..0x1f00e2d8 -> fsel(0|1|4|5, 1) + drive.
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
#define GPIO_GPLR(bank)     REG32(GPIO_BASE + 0x00u + 4u * (bank))  /* level ro */
#define GPIO_GPDR(bank)     REG32(GPIO_BASE + 0x0cu + 4u * (bank))  /* DIR r/w  */
#define GPIO_GPSR(bank)     REG32(GPIO_BASE + 0x18u + 4u * (bank))  /* DATA = 1 */
#define GPIO_GPCR(bank)     REG32(GPIO_BASE + 0x24u + 4u * (bank))  /* DATA = 0 */
#define GPIO_GRER(bank)     REG32(GPIO_BASE + 0x30u + 4u * (bank))  /* rise en  */
#define GPIO_GFER(bank)     REG32(GPIO_BASE + 0x3cu + 4u * (bank))  /* fall en  */
#define GPIO_GEDR(bank)     REG32(GPIO_BASE + 0x48u + 4u * (bank))  /* edge w1c */
#define GPIO_GSDR(bank)     REG32(GPIO_BASE + 0x54u + 4u * (bank))  /* dir=out  */
#define GPIO_GCDR(bank)     REG32(GPIO_BASE + 0x60u + 4u * (bank))  /* dir=in   */
#define GPIO_GSRER(bank)    REG32(GPIO_BASE + 0x6cu + 4u * (bank))
#define GPIO_GCRER(bank)    REG32(GPIO_BASE + 0x78u + 4u * (bank))
#define GPIO_GSFER(bank)    REG32(GPIO_BASE + 0x84u + 4u * (bank))
#define GPIO_GCFER(bank)    REG32(GPIO_BASE + 0x90u + 4u * (bank))

/* --- GPT0 (lamp PWM; regs/gpt_reg.h) ------------------------------------ */
#define GPT_CNT_EN          0x000u   /* counter enable: start/stop/reset     */
#define GPT_CNT_START       (1u << 0)
#define GPT_CNT_STOP        (1u << 1)
#define GPT_CNT_RESET       (1u << 2)
#define GPT_CNT_RUN         (1u << 16)   /* r/o                              */
#define GPT_CNT_RST_DONE    (1u << 17)   /* r/o                              */
#define GPT_STATUS          0x020u
#define GPT_INTR            0x024u
#define GPT_MASK            0x028u
#define GPT_CNT_CNTL        0x040u   /* [9:8] CNT_UPDT_MOD                    */
#define GPT_CNT_UPDT_MOD    8u
#define GPT_CNT_VAL         0x050u
#define GPT_UPPVAL          0x060u   /* counter overflow value = period       */
#define GPT_CLK_CNTL        0x080u   /* [0] src, [11:8] div, [23:16] prescale */
#define GPT_CLK_SRC         0u
#define GPT_CLK_DIV         8u
#define GPT_CLK_PRE         16u
#define GPT_USER_REQ        0x0f0u   /* [5:0] CHn_RST, [21:16] CHn_CMR_UPDT   */
#define GPT_CHAN(ch)        (0x200u + 0x30u * (ch))
#define GPT_CH_CNTL         0x00u    /* [2:0] CH_IO, [14:12] IC_EDGE, [16] POL*/
#define GPT_CH_CMR0         0x10u    /* counter match 0 = duty compare        */
#define GPT_CH_STS          0x14u    /* [0] OUT_ST                            */
#define GPT_CH_CMR1         0x20u    /* counter match 1                       */
#define GPT_CH_IO_MASK      0x7u
#define GPT_CH_IO_PWM_EDGE  6u       /* GPT_CH_FUNC_PWM_EDGE (mw300_gpt.h)    */
#define GPT_CH_IO_PWM_CENTER 7u
#define GPT_CH_POL          (1u << 16)
#define GPT_CMR_UPDT(ch)    (1u << (16u + (ch)))

/* --- interrupt level (mw300_interrupt.h IRQn_Type) ---------------------- */
#define MCUFW_IRQ_EXTPIN0   0        /* ExtPin0 (PMU wakeup pin 0)            */
#define MCUFW_IRQ_EXTPIN1   1        /* ExtPin1 (PMU wakeup pin 1)            */
#define MCUFW_IRQ_GPIO      9        /* GPIO_IRQHandler                       */
#define MCUFW_IRQ_GPT0      14
#define MCUFW_IRQ_UART0     18
#define MCUFW_IRQ_ADC0      24
#define MCUFW_IRQ_COUNT     48

/* Vendor pin-callback table used by GPIO_IRQHandler (mdev_gpio.o):
 * 50 entries (GPIO0..GPIO49) x 8 bytes = 400 bytes, base 0x20017f5c, end
 * 0x200180ec -- the two literals the handler/driver module loads.           */
#define MCUFW_GPIO_IRQ_ARRAY   0x20017f5cu
#define MCUFW_GPIO_IRQ_STRIDE  8u
#define MCUFW_GPIO_PIN_COUNT   50u

/* --- taster / button ----------------------------------------------------
 * The ticket asked for the physical button pin via the interrupt level
 * (intCbfArra).  Result for THIS firmware: there is no GPIO button.
 * Evidence collected with tools/irq_gpt_re.py from color1_0061.elf:
 *   1. The GPIO IRQ path exists: GPIO_IRQHandler @0x1f0379c4 (IRQ 9) reads
 *      GEDR (0x46060048 + bank*4), acks through GCRER/GCFER (0x78/0x90) and
 *      dispatches callbacks from MCUFW_GPIO_IRQ_ARRAY.
 *   2. That array is referenced by exactly one literal in the whole image --
 *      inside the handler's own literal pool at 0x1f037a20.  No registration
 *      site exists => no pin interrupt is ever armed.
 *   3. The level accessor GPIO_GetPinLevel @0x1f0366f2 (reads GPLR) has zero
 *      call sites in the image.
 *   4. No PROGBITS data word anywhere holds a thumb pointer into the GPIO
 *      layer => no indirect registration through a dispatch table either.
 *   => color1 (bulb) has no GPIO taster.  The only input interrupts wired in
 *      the app are the PMU wakeup pins ExtPin0/ExtPin1 (IRQ 0/1, handlers
 *      0x0010181d / 0x00101859), which ack the PMU wakeup status word
 *      (PMU + 0x180, one bit per pin) -- that is the AON/wakeup level, not
 *      GPIO.  Mapping those pads to physical pins is open (docs/09, §4).
 */
#define MCUFW_BUTTON_GPIO_PIN   (-1)  /* proven absent, see block comment  */

#endif /* MCUFW_REGS_H */
