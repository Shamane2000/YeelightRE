/* board.h — mcufw shim for FreeRTOS port
 * Replaces SDK board.h to avoid pulling in the full MW300 register chain.
 * Only provides what FreeRTOS port.c needs: SysTick clock frequency.
 */
#ifndef MCUFW_BOARD_SHIM_H
#define MCUFW_BOARD_SHIM_H

/* MW300 runs at 200 MHz (fixed by PLL config in startup).
 * FreeRTOS port.c uses this only for configSYSTICK_CLOCK_HZ default. */
#define board_cpu_freq()  (200000000UL)

#endif /* MCUFW_BOARD_SHIM_H */
