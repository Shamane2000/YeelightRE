#ifndef MCUFW_OTA_HAL_H
#define MCUFW_OTA_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	int  (*open)(const char *url, int *status_code,
	             uint32_t *content_len, uint8_t *chunked);
	int  (*read)(void *buf, size_t max_len);
	void (*close)(void);
	int  (*slot_begin)(uint32_t *slot_max_size);
	int  (*slot_write)(const uint8_t *data, size_t len);
	int  (*slot_complete)(void);
	void (*slot_abort)(void);
	void (*reboot)(void);
} mcufw_ota_hal_t;

const mcufw_ota_hal_t *mcufw_ota_hal_default(void);

#endif /* MCUFW_OTA_HAL_H */
