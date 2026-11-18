/* startup.c — bare-metal startup for mcufw on Marvell 88MW300/88MW302.
 *
 * Layout contract with mw300_min.ld / the MRVL container (color1_0061):
 *   .isr_vector  -> SRAM0 @ 0x00100000   (container segment 1, type 2)
 *   .text        -> XIP     @ 0x1f0032d8 (container segment 2, type 2, entry)
 *   .data        -> SRAM1   @ 0x20000040 (container segment 3, type 2)
 *
 * boot2 copies each segment to its load address, so .isr_vector really ends up
 * in SRAM0 at 0x00100000; VTOR is repointed there before any C code runs.
 */
#include <stdint.h>

#include "mcufw_regs.h"

extern uint32_t _sidata;   /* .data contents as stored (== 0x20000040) */
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);

void Reset_IRQHandler(void);
void Default_IRQHandler(void);
void HardFault_IRQHandler(void);

typedef void (*mcufw_vector_t)(void);

/* 16 core exceptions + 48 IRQ slots. Unused slots point at
 * Default_IRQHandler so a stray interrupt traps instead of executing garbage. */
#define MCUFW_VECTOR_COUNT 64

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
	Default_IRQHandler,               /* [11] SVCall                      */
	Default_IRQHandler,               /* [12] DebugMonitor                */
	0,                                /* [13] reserved                    */
	Default_IRQHandler,               /* [14] PendSV                      */
	Default_IRQHandler,               /* [15] SysTick                     */
	/* IRQ 0..47 -- all parked on Default_IRQHandler */
#define MCUFW_IRQ_STUB \
	Default_IRQHandler, Default_IRQHandler, Default_IRQHandler, Default_IRQHandler
	MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB,
	MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB,
	MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB, MCUFW_IRQ_STUB
#undef MCUFW_IRQ_STUB
};

/* Placed as the first object in .text by mw300_min.ld, therefore:
 *   e_entry == 0x1f0032d8 | 1 == the "entry" field of the MRVL container. */
__attribute__((section(".text.Reset_IRQHandler"), used))
void Reset_IRQHandler(void)
{
	uint32_t *src = &_sidata;
	uint32_t *dst;

	/* Vector table is the .init segment, relocated to SRAM0 by boot2. */
	REG32(SCB_VTOR) = MCUFW_INIT_BASE;

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
	/* No debug UART guarantee here: trap forever. */
	for (;;)
		;
}

void Default_IRQHandler(void)
{
	for (;;)
		;
}
