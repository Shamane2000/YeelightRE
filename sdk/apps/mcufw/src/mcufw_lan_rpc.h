#ifndef MCUFW_LAN_RPC_H
#define MCUFW_LAN_RPC_H

#include <stdint.h>
#include <stddef.h>

/* LAN JSON-RPC on TCP 55443 (Yeelight compatibility channel).
 * Whitelist from color1_0061: set_ps/get_ps/cfg_lan_ctrl/ota are BLOCKED
 * on this port (code 2), they only work via miIO UDP 54321. */

#define MCUFW_LAN_RPC_PORT 55443

/* Return codes matching the Yeelight JSON-RPC spec. */
#define MCUFW_RPC_OK          0
#define MCUFW_RPC_E_PARSE    -1
#define MCUFW_RPC_E_METHOD   -2   /* code 2: method not supported on LAN */
#define MCUFW_RPC_E_PARAM    -3
#define MCUFW_RPC_E_INTERNAL -4

typedef struct {
    char method[32];
    char params[256];
    uint32_t id;
    int have_id;
} mcufw_rpc_request_t;

/* Parse a single JSON-RPC request from a raw buffer.
 * Returns MCUFW_RPC_OK on success, negative on parse error. */
int mcufw_rpc_parse(const char *buf, size_t len, mcufw_rpc_request_t *req);

/* Check if a method is allowed on the LAN RPC port.
 * Returns 1 if allowed, 0 if blocked (must go through miIO). */
int mcufw_rpc_method_allowed(const char *method);

/* Dispatch a parsed request and write the JSON response into out.
 * Returns bytes written, or negative on error. */
int mcufw_rpc_dispatch(const mcufw_rpc_request_t *req,
                       char *out, size_t out_cap);

#endif /* MCUFW_LAN_RPC_H */
