/*
 * mcufw_devcfg.h -- Persistent extended device configuration (Phase 4b.3).
 *
 * Holds Wi-Fi credentials, MQTT broker settings, power-on behavior, PWM
 * anti-flicker frequency, default transition time, and gamma flag in a
 * CRC-32-protected binary record persisted via sys_psm ("mcufw.devcfg").
 */
#ifndef MCUFW_DEVCFG_H
#define MCUFW_DEVCFG_H

#include <stddef.h>
#include <stdint.h>

#include "mcufw_state.h"

#define MCUFW_DEVCFG_MAGIC      0x4643444Du /* "MDCF" */
#define MCUFW_DEVCFG_VERSION    1u
#define MCUFW_DEVCFG_WIRE_SIZE  224u

#define MCUFW_POWERON_LAST        0u
#define MCUFW_POWERON_OFF         1u
#define MCUFW_POWERON_DEFAULT     2u

#define MCUFW_PWM_FREQ_MIN      500u
#define MCUFW_PWM_FREQ_MAX      20000u
#define MCUFW_PWM_FREQ_DEFAULT  768u
#define MCUFW_TRANS_MAX_MS      5000u

typedef struct {
	char     wifi_ssid[33];
	char     wifi_psk[64];
	uint8_t  wifi_provisioned;
	char     mqtt_host[40];
	uint16_t mqtt_port;
	char     mqtt_user[32];
	char     mqtt_pass[32];
	uint8_t  mqtt_enabled;
	uint8_t  power_on_mode;
	uint16_t pwm_freq_hz;
	uint16_t default_transition_ms;
	uint8_t  gamma_enable;
} mcufw_devcfg_t;

void mcufw_devcfg_defaults(mcufw_devcfg_t *cfg);
int  mcufw_devcfg_boot(void);
int  mcufw_devcfg_save(void);
mcufw_devcfg_t *mcufw_devcfg(void);
const char *mcufw_devcfg_poweron_name(uint8_t mode);

/* Adjust state according to configured power_on_mode before mcufw_state_apply(). */
int mcufw_devcfg_apply_power_on(mcufw_state_t *st);

/* Serialize config as JSON, masking wifi_psk and mqtt_pass as "***". */
int mcufw_devcfg_to_json(const mcufw_devcfg_t *cfg, char *out, size_t cap);

/* Parse JSON keys onto a staging copy, validate, persist if valid.
 * Returns 0 on success, -1 on validation/parse failure (active cfg unchanged). */
int mcufw_devcfg_apply_json(const char *json, size_t len);

/* Explicit field setters (validated & persisted) for HA MQTT / RPC controls. */
int mcufw_devcfg_set_power_on_mode(uint8_t mode);
int mcufw_devcfg_set_pwm_freq(uint16_t hz);
int mcufw_devcfg_set_transition_ms(uint16_t ms);

size_t mcufw_devcfg_encode(const mcufw_devcfg_t *cfg, uint8_t *out, size_t cap);
int    mcufw_devcfg_decode(const uint8_t *in, size_t len, mcufw_devcfg_t *out);

#endif /* MCUFW_DEVCFG_H */
