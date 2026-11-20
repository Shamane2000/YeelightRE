/*
 * mcufw_http_portal.h -- Local Setup-AP / LAN HTTP Config & OTA portal (port 8080).
 *
 * Pure byte-buffer HTTP/1.1 handler: zero sockets, zero RTOS, host-testable.
 */
#ifndef MCUFW_HTTP_PORTAL_H
#define MCUFW_HTTP_PORTAL_H

#include <stddef.h>
#include <stdint.h>

#define MCUFW_HTTP_PORT 8080u

int mcufw_http_handle_request(const char *req, size_t req_len,
                              char *rsp, size_t rsp_cap, size_t *rsp_len);

#endif /* MCUFW_HTTP_PORTAL_H */
