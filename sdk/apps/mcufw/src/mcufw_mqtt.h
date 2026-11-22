/*
 * mcufw_mqtt.h -- MQTT 3.1.1 client + Home Assistant 2026.11 Auto-Discovery.
 *
 * Phase 3.5 serialization helpers (mcufw_mqtt_discovery_light,
 * mcufw_mqtt_state_payload, etc.) are preserved verbatim.
 *
 * Phase 4b.4 adds:
 *   - Clean-room MQTT 3.1.1 wire codec & non-blocking client state machine
 *   - Injectable I/O seam (lwIP raw TCP on target, loopback broker in tests)
 *   - Full HA 2026.11 Auto-Discovery (light, update, config select/number/
 *     buttons, diagnostic sensor) sharing one device block
 *   - Bidirectional LAN-RPC 55443 <-> MQTT state sync via mcufw_state()
 */
#ifndef MCUFW_MQTT_H
#define MCUFW_MQTT_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_MQTT_PORT            1883u
#define MCUFW_MQTT_FW_VERSION      "0.8-phase4b.4"
#define MCUFW_MQTT_KEEPALIVE_S     30u
#define MCUFW_MQTT_BACKOFF_MIN_MS  2000u
#define MCUFW_MQTT_BACKOFF_MAX_MS  30000u

typedef enum {
	MCUFW_MQTT_ST_DISABLED = 0,
	MCUFW_MQTT_ST_DISCONNECTED,
	MCUFW_MQTT_ST_TCP_CONNECTING,
	MCUFW_MQTT_ST_WAIT_CONNACK,
	MCUFW_MQTT_ST_DISCOVERY_BURST,
	MCUFW_MQTT_ST_SUBSCRIBE,
	MCUFW_MQTT_ST_CONNECTED,
	MCUFW_MQTT_ST_BACKOFF,
} mcufw_mqtt_conn_state_t;

typedef struct {
	int      (*link_ready)(void);
	int      (*tcp_connect)(const char *host, uint16_t port);
	int      (*tcp_state)(void); /* 0 = closed/err, 1 = connecting, 2 = connected */
	int      (*tcp_send)(const uint8_t *buf, size_t len);
	int      (*tcp_recv)(uint8_t *buf, size_t cap);
	void     (*tcp_close)(void);
	uint32_t (*now_ms)(void);
	int      (*diag_info)(char *ip_out, size_t ip_cap, int *rssi_out,
	                      uint8_t *active_slot_out);
} mcufw_mqtt_io_t;

typedef struct {
	mcufw_mqtt_conn_state_t state;
	uint32_t connects;
	uint32_t disconnects;
	uint32_t pub_tx;
	uint32_t pub_rx;
	uint32_t ping_tx;
	uint32_t ping_rx;
	uint32_t suback_rx;
	uint32_t backoff_ms;
	uint32_t last_rx_ms;
	int      restart_pending;
} mcufw_mqtt_client_stats_t;

/* Phase 3.5 payload & topic builders (host-tested, kept verbatim). */
int mcufw_mqtt_discovery_light(uint32_t muid, char *out, size_t cap);
int mcufw_mqtt_state_payload(int power, uint8_t bright, uint16_t ct,
                             uint16_t hue, uint8_t sat,
                             char *out, size_t cap);
int mcufw_mqtt_discovery_topic(uint32_t muid, char *out, size_t cap);
int mcufw_mqtt_state_topic(uint32_t muid, char *out, size_t cap);
int mcufw_mqtt_cmd_topic(uint32_t muid, char *out, size_t cap);

/* Phase 4b.4 client API. */
void mcufw_mqtt_client_init(uint32_t muid);
void mcufw_mqtt_client_bind(const mcufw_mqtt_io_t *io);
void mcufw_mqtt_client_tick(void);
const mcufw_mqtt_client_stats_t *mcufw_mqtt_client_stats(void);

#endif /* MCUFW_MQTT_H */
