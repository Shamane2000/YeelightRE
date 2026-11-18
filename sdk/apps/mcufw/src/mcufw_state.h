/*
 * mcufw_state.h -- internal lamp state model (phase 3.6).
 *
 * One authoritative struct holds everything the lamp "is": power, brightness,
 * colour temperature, HSV, name, colour-flow status and the default flag.
 * The LAN-RPC (TCP 55443) and miIO (UDP 54321) dispatchers do NOT keep their
 * own copies -- they mutate this state through the setters below, and the
 * setters push the result into the PWM backend (mcufw_pwm.c).
 *
 * Every setter clamps into the ranges the reference firmware reports
 * (color1_0061: bright 1..100, ct 1700..6500, hue 0..359, sat 0..100), so a
 * client can never park the lamp in an undefined state.
 *
 * The module is free of libc, RTOS and register access: identical code on the
 * target and in the host test.
 */
#ifndef MCUFW_STATE_H
#define MCUFW_STATE_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_BRIGHT_MIN   1
#define MCUFW_BRIGHT_MAX   100
#define MCUFW_CT_MIN       1700
#define MCUFW_CT_MAX       6500
#define MCUFW_HUE_MAX      359
#define MCUFW_SAT_MAX      100
#define MCUFW_NAME_MAX     32

/* Yeelight `color_mode`: 2 = colour temperature, 3 = HSV. */
#define MCUFW_COLOR_MODE_CT   2u
#define MCUFW_COLOR_MODE_HSV  3u

typedef struct {
	uint8_t  power;      /* 0 = off, 1 = on                             */
	uint8_t  bright;     /* 1..100 (also the HSV V component)           */
	uint16_t ct;         /* 1700..6500 Kelvin                           */
	uint16_t hue;        /* 0..359                                      */
	uint8_t  sat;        /* 0..100                                      */
	uint8_t  color_mode; /* 2 = ct, 3 = hsv                             */
	uint8_t  cf_active;  /* colour flow running                         */
	uint8_t  is_default; /* "set_default" marker                        */
	char     name[MCUFW_NAME_MAX + 1u];
	uint32_t mutations;  /* applied state changes (tests/telemetry)     */
} mcufw_state_t;

/* Reset to the reference power-on state: off, 100 %, 4000 K, mode ct. */
void mcufw_state_init(mcufw_state_t *st);

/* Process-global lamp state, lazily initialised on first use. */
mcufw_state_t *mcufw_state(void);

/* Clamp helpers (exposed so a test can assert the exact edges). */
uint8_t  mcufw_state_clamp_bright(long v);
uint16_t mcufw_state_clamp_ct(long v);
uint16_t mcufw_state_clamp_hue(long v);
uint8_t  mcufw_state_clamp_sat(long v);

/* Setters: clamp, count the mutation, push the result to the PWM backend.
 * Return 0 on success, negative on a NULL state. */
int mcufw_state_set_power(mcufw_state_t *st, int on);
int mcufw_state_toggle(mcufw_state_t *st);
int mcufw_state_set_bright(mcufw_state_t *st, long bright);
int mcufw_state_set_ct(mcufw_state_t *st, long ct);
int mcufw_state_set_hsv(mcufw_state_t *st, long hue, long sat, long val);
int mcufw_state_set_name(mcufw_state_t *st, const char *name);
int mcufw_state_set_cf(mcufw_state_t *st, int active);
int mcufw_state_set_default(mcufw_state_t *st, int is_default);

/* Translate the current state into PWM calls (off / ct / hsv). */
void mcufw_state_apply(mcufw_state_t *st);

#endif /* MCUFW_STATE_H */
