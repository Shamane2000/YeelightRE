/*
 * mcufw_wmsdk_api.h -- DWARF-verified Marvell WMSDK / EZ-Connect-Lite ABI.
 *
 * The precompiled libwmsdk.a snapshot omits partition.h, psm-v2.h, rfget.h,
 * and httpc.h from sdk/src/incl/. Every struct layout, enum value, and
 * function prototype below was verified directly against the DWARF .debug_info
 * sections of partition.o, psm-v2.o, app_psm.o, psm-utils.o, rfget.o, httpc.o.
 */
#ifndef MCUFW_WMSDK_API_H
#define MCUFW_WMSDK_API_H

#include <stddef.h>
#include <stdint.h>

enum flash_comp {
	FC_COMP_BOOT2    = 0,
	FC_COMP_FW       = 1,
	FC_COMP_WLAN_FW  = 2,
	FC_COMP_FTFS     = 3,
	FC_COMP_PSM      = 4,
	FC_COMP_USER_APP = 5,
};

typedef struct flash_desc {
	uint8_t  fl_dev;
	uint8_t  _pad[3];
	uint32_t fl_start;
	uint32_t fl_size;
} flash_desc_t;

struct partition_entry {
	uint8_t  type;
	uint8_t  device;
	char     name[10];
	uint32_t start;
	uint32_t size;
	uint32_t gen_level;
};

typedef struct update_desc {
	struct partition_entry *pe;
	void                   *dev;
	uint32_t                addr;
	uint32_t                remaining;
} update_desc_t;

typedef struct {
	const char *protocol;
	int         version;
	int         status_code;
	const char *reason_phrase;
	const char *server;
	const char *content_type;
	const char *content_encoding;
	uint8_t     keep_alive_ack;
	uint8_t     _pad1[3];
	int         keep_alive_timeout;
	int         keep_alive_max;
	uint8_t     chunked;
	uint8_t     content_length_field_present;
	uint8_t     _pad2[2];
	uint32_t    content_length;
} mcufw_http_resp_t;

typedef void *psm_hnd_t;
typedef int   http_session_t;

_Static_assert(sizeof(flash_desc_t) == 12u, "flash_desc_t ABI mismatch");
_Static_assert(sizeof(struct partition_entry) == 24u, "partition_entry ABI mismatch");
_Static_assert(offsetof(struct partition_entry, start) == 12u, "partition_entry.start mismatch");
_Static_assert(offsetof(struct partition_entry, gen_level) == 20u, "partition_entry.gen_level mismatch");
_Static_assert(sizeof(update_desc_t) == 16u, "update_desc_t ABI mismatch");
_Static_assert(sizeof(mcufw_http_resp_t) == 48u, "http_resp_t size mismatch");
_Static_assert(offsetof(mcufw_http_resp_t, status_code) == 8u, "http_resp_t.status_code mismatch");
_Static_assert(offsetof(mcufw_http_resp_t, chunked) == 40u, "http_resp_t.chunked mismatch");
_Static_assert(offsetof(mcufw_http_resp_t, content_length) == 44u, "http_resp_t.content_length mismatch");

/* Core / Partition API */
int wm_core_init(void);
int part_init(void);
void part_to_flash_desc(struct partition_entry *p, flash_desc_t *f);
struct partition_entry *part_get_layout_by_id(enum flash_comp comp, int16_t *start_index);
struct partition_entry *part_get_active_partition(struct partition_entry *p1,
                                                  struct partition_entry *p2);
struct partition_entry *part_get_passive_partition(struct partition_entry *p1,
                                                   struct partition_entry *p2);
struct partition_entry *part_get_passive_partition_by_name(const char *name);
int part_set_active_partition(struct partition_entry *p);

/* Sys-PSM API */
int sys_psm_init(void);
psm_hnd_t sys_psm_get_handle(void);
int psm_set_variable(psm_hnd_t phandle, const char *variable,
                     const void *value, unsigned long len);
int psm_get_variable(psm_hnd_t phandle, const char *variable,
                     void *value, unsigned long max_len);

/* RFGET / Flash OTA API */
int rfget_init(void);
struct partition_entry *rfget_get_passive_firmware(void);
int rfget_update_begin(update_desc_t *ud, struct partition_entry *p);
int rfget_update_data(update_desc_t *ud, const char *data, size_t datalen);
int rfget_update_complete(update_desc_t *ud);
int rfget_update_abort(update_desc_t *ud);

/* HTTP Client API */
int httpc_get(const char *url_str, http_session_t *handle,
              mcufw_http_resp_t **http_resp, const void *httpc_cfg);
int http_read_content(http_session_t handle, void *buf, uint32_t max_len);
void http_close_session(http_session_t *handle);

#endif /* MCUFW_WMSDK_API_H */
