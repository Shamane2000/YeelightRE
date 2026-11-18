#ifndef MCUFW_MQTT_H
#define MCUFW_MQTT_H

#include <stdint.h>
#include <stddef.h>

/* MQTT + HA Auto-Discovery for mcufw phase 3.5.
 * Broker config lives in NVRAM (mcufw_wlan_cfg_t extension or separate struct).
 * Discovery topics: homeassistant/light/<muid>/{light,switch}/config
 * State/command topics: yeelight/<muid>/state, yeelight/<muid>/set
 *
 * Phase 3.5: packet serialization + discovery payload generation only.
 * Actual TCP connect + publish loop comes in phase 5 with real WLAN assoc.
 */

#define MCUFW_MQTT_PORT 1883

/* Build an HA auto-discovery config payload for a light entity.
 * Returns bytes written into out (excluding NUL). */
int mcufw_mqtt_discovery_light(uint32_t muid, char *out, size_t cap);

/* Build a state report JSON payload.
 * Returns bytes written. */
int mcufw_mqtt_state_payload(int power, uint8_t bright, uint16_t ct,
                             uint16_t hue, uint8_t sat,
                             char *out, size_t cap);

/* Format the discovery topic string.
 * Returns bytes written. */
int mcufw_mqtt_discovery_topic(uint32_t muid, char *out, size_t cap);

/* Format the state topic string. */
int mcufw_mqtt_state_topic(uint32_t muid, char *out, size_t cap);

/* Format the command topic string. */
int mcufw_mqtt_cmd_topic(uint32_t muid, char *out, size_t cap);

#endif /* MCUFW_MQTT_H */
