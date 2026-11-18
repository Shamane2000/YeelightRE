/* mcufw_gpt.h — GPT0 PWM driver for the Yeelight lamp channels.
 *
 * The lamp of color1 is not a plain GPIO LED: color1_0061.elf drives four
 * GPT0 PWM channels (CH0/CH1/CH4/CH5 on GPIO0/1/4/5, pin mux FSEL 1) and
 * updates them through the vendor dispatch FUN_1f0110a0 (mode 1/2/3, vtable
 * slot +0x44, 200 ms led timer).  mcufw replicates the register level of that
 * path without the vendor driver object.
 */
#ifndef MCUFW_GPT_H
#define MCUFW_GPT_H

#include <stdint.h>

/* Period proven from the app init table at 0x1f00f1c4: every channel entry
 * carries 0xC350 (50000) as the counter overflow value. */
#define MCUFW_LAMP_PERIOD   0xc350u

#define MCUFW_LAMP_CHANNELS 4

/* Configure GPT0 + the four lamp channels, counter stopped at level 0. */
void mcufw_gpt0_pwm_init(void);

/* level: 0 = lamp channel off, MCUFW_LAMP_PERIOD = full on. */
void mcufw_lamp_set(uint16_t level);

/* Readback of the last programmed level and of the hardware CMR0 registers
 * (one per lamp channel, index 0..MCUFW_LAMP_CHANNELS-1) so a live UART test
 * can verify the driver without a debugger. */
uint16_t mcufw_lamp_get(void);
uint32_t mcufw_lamp_cmr0(unsigned index);
uint32_t mcufw_lamp_counter(void);

#endif /* MCUFW_GPT_H */
