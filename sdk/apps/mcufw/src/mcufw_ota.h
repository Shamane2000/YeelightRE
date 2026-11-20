/*
 * mcufw_ota.h -- Integrated safe Self-OTA state machine (Phase 4b.3).
 *
 * Streams an HTTP firmware image directly into the passive WMPT app slot:
 *   1. Validates URL ("http://...") & optional 32-char MD5 before network I/O
 *   2. Opens HTTP stream & verifies HTTP 200 + non-chunked + MRVL header
 *      BEFORE erasing/touching the passive flash partition
 *   3. Streams <= slot_max bytes into passive slot while hashing MD5
 *   4. Aborts without slot swap on any I/O, length, magic, or MD5 failure
 *   5. Calls rfget_update_complete(), clears rollback boot_cnt, and reboots
 */
#ifndef MCUFW_OTA_H
#define MCUFW_OTA_H

#include <stddef.h>
#include <stdint.h>

#include "mcufw_ota_hal.h"

#define MCUFW_OTA_URL_MAX  160u
#define MCUFW_OTA_MD5_HEX  32u

typedef enum {
	MCUFW_OTA_IDLE    = 0,
	MCUFW_OTA_QUEUED  = 1,
	MCUFW_OTA_RUNNING = 2,
	MCUFW_OTA_DONE    = 3,
	MCUFW_OTA_ERR     = 4,
} mcufw_ota_status_t;

typedef struct {
	mcufw_ota_status_t status;
	uint32_t           bytes_written;
	uint32_t           bytes_total;
	char               last_error[32];
	char               last_url[MCUFW_OTA_URL_MAX];
	int                rollback_pending;
} mcufw_ota_state_t;

void mcufw_ota_set_hal(const mcufw_ota_hal_t *hal);
void mcufw_ota_reset(void);

/* Queue an OTA download (non-blocking, safe from miIO/MQTT/HTTP handlers).
 * Returns 0 on accept, -1 on invalid URL/MD5 or busy state. */
int mcufw_ota_queue(const char *url, const char *md5_hex);

/* Schedule safe rollback to passive WMPT slot after a short tick delay
 * so HTTP/MQTT acknowledgements can flush before AIRCR reset. */
void mcufw_ota_request_rollback(void);

/* Execute synchronous OTA update (called by mcufw_ota_service() or host tests).
 * Returns 0 on complete+reboot trigger, negative on abort. */
int mcufw_ota_execute(const char *url, const char *md5_hex);

/* Called once per runtime tick: processes rollback countdown or queued OTA. */
int mcufw_ota_service(void);

const mcufw_ota_state_t *mcufw_ota_state(void);
const char *mcufw_ota_status_name(mcufw_ota_status_t s);

#endif /* MCUFW_OTA_H */
