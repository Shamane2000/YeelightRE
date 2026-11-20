/* startup.c -- bare-metal startup for mcufw on Marvell 88MW300/88MW302.
 *
 * Phase 4b.2:
 *   - 79-Slot-Vektortabelle (16 Cortex-M4F Core-Exceptions + 63 IRQs gemäß
 *     mw300_interrupt.h, IRQ 0..62).
 *   - Core-Slots [11]/[14]/[15] auf echte FreeRTOS-Handler aus port.o:
 *     SVC_IRQHandler, PendSV_IRQHandler, SysTick_IRQHandler.
 *   - Alle realen HW-IRQ-Handler aus libwmsdk_mcufw.a verdrahtet (inkl.
 *     SDIO_IRQHandler [44] / WIFIWKUP_IRQHandler [78] / DMA / QSPI / GPT / UART).
 *   - FPU (CP10/CP11) vor main() in Reset_IRQHandler freigeschaltet.
 */
#include <stdint.h>

#include "mcufw_regs.h"

#define SCB_CPACR_REG 0xE000ED88u

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);

void Reset_IRQHandler(void);
void Default_IRQHandler(void);
void HardFault_IRQHandler(void);

#ifndef HOST_TEST
/* FreeRTOS Cortex-M4F port handlers (echte T-Symbole aus port.o) */
extern void SVC_IRQHandler(void);
extern void PendSV_IRQHandler(void);
extern void SysTick_IRQHandler(void);

/* Reale HW-IRQ-Handler aus libwmsdk_mcufw.a (alle 27 T-Symbole) */
extern void ExtPin0_IRQHandler(void);   /* IRQ 0  */
extern void ExtPin1_IRQHandler(void);   /* IRQ 1  */
extern void RTC_IRQHandler(void);       /* IRQ 2  */
extern void CRC_IRQHandler(void);       /* IRQ 3  */
extern void AES_IRQHandler(void);       /* IRQ 4  */
extern void I2C0_IRQHandler(void);      /* IRQ 5  */
extern void I2C1_IRQHandler(void);      /* IRQ 6  */
extern void DMA_IRQHandler(void);       /* IRQ 8  */
extern void GPIO_IRQHandler(void);      /* IRQ 9  */
extern void SSP0_IRQHandler(void);      /* IRQ 10 */
extern void SSP1_IRQHandler(void);      /* IRQ 11 */
extern void SSP2_IRQHandler(void);      /* IRQ 12 */
extern void QSPI_IRQHandler(void);      /* IRQ 13 */
extern void GPT0_IRQHandler(void);      /* IRQ 14 */
extern void GPT1_IRQHandler(void);      /* IRQ 15 */
extern void GPT2_IRQHandler(void);      /* IRQ 16 */
extern void GPT3_IRQHandler(void);      /* IRQ 17 */
extern void UART0_IRQHandler(void);     /* IRQ 18 */
extern void UART1_IRQHandler(void);     /* IRQ 19 */
extern void UART2_IRQHandler(void);     /* IRQ 20 */
extern void WDT_IRQHandler(void);       /* IRQ 22 */
extern void ADC0_IRQHandler(void);      /* IRQ 24 */
extern void DAC_IRQHandler(void);       /* IRQ 25 */
extern void ACOMP_IRQHandler(void);     /* IRQ 27 */
extern void SDIO_IRQHandler(void);      /* IRQ 28 */
extern void RC32M_IRQHandler(void);     /* IRQ 33 */
extern void WIFIWKUP_IRQHandler(void);  /* IRQ 62 */
#else
static void SVC_IRQHandler(void) {}
static void PendSV_IRQHandler(void) {}
static void SysTick_IRQHandler(void) {}
static void ExtPin0_IRQHandler(void) {}
static void ExtPin1_IRQHandler(void) {}
static void RTC_IRQHandler(void) {}
static void CRC_IRQHandler(void) {}
static void AES_IRQHandler(void) {}
static void I2C0_IRQHandler(void) {}
static void I2C1_IRQHandler(void) {}
static void DMA_IRQHandler(void) {}
static void GPIO_IRQHandler(void) {}
static void SSP0_IRQHandler(void) {}
static void SSP1_IRQHandler(void) {}
static void SSP2_IRQHandler(void) {}
static void QSPI_IRQHandler(void) {}
static void GPT0_IRQHandler(void) {}
static void GPT1_IRQHandler(void) {}
static void GPT2_IRQHandler(void) {}
static void GPT3_IRQHandler(void) {}
static void UART0_IRQHandler(void) {}
static void UART1_IRQHandler(void) {}
static void UART2_IRQHandler(void) {}
static void WDT_IRQHandler(void) {}
static void ADC0_IRQHandler(void) {}
static void DAC_IRQHandler(void) {}
static void ACOMP_IRQHandler(void) {}
static void SDIO_IRQHandler(void) {}
static void RC32M_IRQHandler(void) {}
static void WIFIWKUP_IRQHandler(void) {}
#endif

typedef void (*mcufw_vector_t)(void);

/* 79 Slots = 16 core + 63 IRQs (IRQ 0..62 per mw300_interrupt.h) */
#define MCUFW_VECTOR_COUNT 79

__attribute__((section(".isr_vector"), used))
const mcufw_vector_t mcufw_vectors[MCUFW_VECTOR_COUNT] = {
	(mcufw_vector_t)MCUFW_STACK_TOP,  /* [0] initial MSP                  */
	Reset_IRQHandler,                 /* [1] reset                        */
	Default_IRQHandler,               /* [2] NMI                          */
	HardFault_IRQHandler,             /* [3] HardFault                    */
	Default_IRQHandler,               /* [4] MemManage                    */
	Default_IRQHandler,               /* [5] BusFault                     */
	Default_IRQHandler,               /* [6] UsageFault                   */
	0, 0, 0, 0,                       /* [7..10] reserved                 */
	SVC_IRQHandler,                   /* [11] SVCall -> FreeRTOS          */
	Default_IRQHandler,               /* [12] DebugMonitor                */
	0,                                /* [13] reserved                    */
	PendSV_IRQHandler,                /* [14] PendSV -> FreeRTOS          */
	SysTick_IRQHandler,               /* [15] SysTick -> FreeRTOS         */
	/* IRQ 0..62 (Index 16..78) */
	ExtPin0_IRQHandler,               /* [16] IRQ 0  ExtPin0              */
	ExtPin1_IRQHandler,               /* [17] IRQ 1  ExtPin1              */
	RTC_IRQHandler,                   /* [18] IRQ 2  RTC                  */
	CRC_IRQHandler,                   /* [19] IRQ 3  CRC                  */
	AES_IRQHandler,                   /* [20] IRQ 4  AES                  */
	I2C0_IRQHandler,                  /* [21] IRQ 5  I2C0                 */
	I2C1_IRQHandler,                  /* [22] IRQ 6  I2C1                 */
	Default_IRQHandler,               /* [23] IRQ 7  reserved             */
	DMA_IRQHandler,                   /* [24] IRQ 8  DMA                  */
	GPIO_IRQHandler,                  /* [25] IRQ 9  GPIO                 */
	SSP0_IRQHandler,                  /* [26] IRQ 10 SSP0                 */
	SSP1_IRQHandler,                  /* [27] IRQ 11 SSP1                 */
	SSP2_IRQHandler,                  /* [28] IRQ 12 SSP2                 */
	QSPI_IRQHandler,                  /* [29] IRQ 13 QSPI                 */
	GPT0_IRQHandler,                  /* [30] IRQ 14 GPT0                 */
	GPT1_IRQHandler,                  /* [31] IRQ 15 GPT1                 */
	GPT2_IRQHandler,                  /* [32] IRQ 16 GPT2                 */
	GPT3_IRQHandler,                  /* [33] IRQ 17 GPT3                 */
	UART0_IRQHandler,                 /* [34] IRQ 18 UART0                */
	UART1_IRQHandler,                 /* [35] IRQ 19 UART1                */
	UART2_IRQHandler,                 /* [36] IRQ 20 UART2                */
	Default_IRQHandler,               /* [37] IRQ 21 reserved             */
	WDT_IRQHandler,                   /* [38] IRQ 22 WDT                  */
	Default_IRQHandler,               /* [39] IRQ 23 reserved             */
	ADC0_IRQHandler,                  /* [40] IRQ 24 ADC0                 */
	DAC_IRQHandler,                   /* [41] IRQ 25 DAC                  */
	Default_IRQHandler,               /* [42] IRQ 26 reserved             */
	ACOMP_IRQHandler,                 /* [43] IRQ 27 ACOMP                */
	SDIO_IRQHandler,                  /* [44] IRQ 28 SDIO                 */
	Default_IRQHandler,               /* [45] IRQ 29 USB                  */
	Default_IRQHandler,               /* [46] IRQ 30 reserved             */
	Default_IRQHandler,               /* [47] IRQ 31 PLL                  */
	Default_IRQHandler,               /* [48] IRQ 32 reserved             */
	RC32M_IRQHandler,                 /* [49] IRQ 33 RC32M                */
	Default_IRQHandler,               /* [50] IRQ 34 ExtPin3              */
	Default_IRQHandler,               /* [51] IRQ 35 ExtPin4              */
	Default_IRQHandler,               /* [52] IRQ 36 ExtPin5              */
	Default_IRQHandler,               /* [53] IRQ 37 ExtPin6              */
	Default_IRQHandler,               /* [54] IRQ 38 ExtPin7              */
	Default_IRQHandler,               /* [55] IRQ 39 ExtPin8              */
	Default_IRQHandler,               /* [56] IRQ 40 ExtPin9              */
	Default_IRQHandler,               /* [57] IRQ 41 ExtPin10             */
	Default_IRQHandler,               /* [58] IRQ 42 ExtPin11             */
	Default_IRQHandler,               /* [59] IRQ 43 ExtPin12             */
	Default_IRQHandler,               /* [60] IRQ 44 ExtPin13             */
	Default_IRQHandler,               /* [61] IRQ 45 ExtPin14             */
	Default_IRQHandler,               /* [62] IRQ 46 ExtPin15             */
	Default_IRQHandler,               /* [63] IRQ 47 ExtPin16             */
	Default_IRQHandler,               /* [64] IRQ 48 ExtPin17             */
	Default_IRQHandler,               /* [65] IRQ 49 ExtPin18             */
	Default_IRQHandler,               /* [66] IRQ 50 ExtPin19             */
	Default_IRQHandler,               /* [67] IRQ 51 ExtPin20             */
	Default_IRQHandler,               /* [68] IRQ 52 ExtPin21             */
	Default_IRQHandler,               /* [69] IRQ 53 ExtPin22             */
	Default_IRQHandler,               /* [70] IRQ 54 ExtPin23             */
	Default_IRQHandler,               /* [71] IRQ 55 ExtPin24             */
	Default_IRQHandler,               /* [72] IRQ 56 ExtPin25             */
	Default_IRQHandler,               /* [73] IRQ 57 ExtPin26             */
	Default_IRQHandler,               /* [74] IRQ 58 ExtPin27             */
	Default_IRQHandler,               /* [75] IRQ 59 reserved             */
	Default_IRQHandler,               /* [76] IRQ 60 reserved             */
	Default_IRQHandler,               /* [77] IRQ 61 BRNDET               */
	WIFIWKUP_IRQHandler,              /* [78] IRQ 62 WIFIWKUP             */
};

__attribute__((section(".text.Reset_IRQHandler"), used))
void Reset_IRQHandler(void)
{
	uint32_t *src = &_sidata;
	uint32_t *dst;

	REG32(SCB_VTOR) = MCUFW_INIT_BASE;

	/* Enable Cortex-M4F FPU (CP10 + CP11 full access) before C code runs. */
	REG32(SCB_CPACR_REG) |= (0xFu << 20);
	__asm volatile ("dsb 0xF" ::: "memory");
	__asm volatile ("isb 0xF" ::: "memory");

	for (dst = &_sdata; dst < &_edata; )
		*dst++ = *src++;
	for (dst = &_sbss; dst < &_ebss; )
		*dst++ = 0;

	(void)main();

	for (;;)
		;
}

__attribute__((section(".text.HardFault_IRQHandler"), used))
void HardFault_IRQHandler(void)
{
	for (;;)
		;
}

void Default_IRQHandler(void)
{
	for (;;)
		;
}
