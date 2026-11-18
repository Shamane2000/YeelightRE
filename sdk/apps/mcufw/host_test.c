/*
 * host_test.c -- mcufw phase 3.5 host test suite.
 *
 * Covers:
 *   - Crypto + miIO selftest (from phase 3.4)
 *   - LAN-RPC whitelist enforcement (blocked vs allowed methods)
 *   - MQTT discovery/state payload serialization
 *   - Color Flow parameter parsing
 *
 * Exit code 0 = ALL PASS.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mcufw_selftest.h"
#include "mcufw_lan_rpc.h"
#include "mcufw_mqtt.h"
#include "mcufw_cf.h"

static unsigned total_pass;
static unsigned total_fail;

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  %-40s PASS\n", name);
        total_pass++;
    } else {
        printf("  %-40s FAIL\n", name);
        total_fail++;
    }
}

/* ---- crypto + miIO selftest (phase 3.4) --------------------------------- */
static void run_selftest(void)
{
    uint32_t fail = mcufw_selftest();
    unsigned n = mcufw_selftest_count();

    for (unsigned i = 0; i < n; i++) {
        int ok = (fail & (1u << i)) == 0u;
        char label[64];
        snprintf(label, sizeof(label), "selftest/%s", mcufw_selftest_name(i));
        check(label, ok);
    }
}

/* ---- LAN-RPC whitelist -------------------------------------------------- */
static void run_rpc_whitelist(void)
{
    /* Blocked methods must return code 2 on LAN RPC port */
    const char *blocked[] = {"set_ps", "get_ps", "cfg_lan_ctrl", "ota"};
    for (unsigned i = 0; i < 4; i++) {
        char label[64];
        snprintf(label, sizeof(label), "rpc/blocked/%s", blocked[i]);
        check(label, mcufw_rpc_method_allowed(blocked[i]) == 0);
    }

    /* Allowed methods must pass the whitelist gate */
    const char *allowed[] = {"set_power", "toggle", "set_bright",
                             "set_ct_abx", "set_hsv", "get_prop",
                             "start_cf", "set_name", "set_default"};
    for (unsigned i = 0; i < 9; i++) {
        char label[64];
        snprintf(label, sizeof(label), "rpc/allowed/%s", allowed[i]);
        check(label, mcufw_rpc_method_allowed(allowed[i]) == 1);
    }
}

/* ---- LAN-RPC dispatch --------------------------------------------------- */
static void run_rpc_dispatch(void)
{
    char out[256];
    mcufw_rpc_request_t req;
    int rc;

    /* Blocked method -> code 2 error response */
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_ps");
    req.id = 1;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/dispatch/blocked_set_ps", rc > 0 && strstr(out, "\"code\":2") != NULL);

    /* Allowed method -> ok response */
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_power");
    req.id = 2;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/dispatch/allowed_set_power", rc > 0 && strstr(out, "\"ok\"") != NULL);

    /* Parse test: valid JSON-RPC request */
    const char *json = "{\"id\":3,\"method\":\"get_prop\",\"params\":[\"power\"]}";
    memset(&req, 0, sizeof(req));
    rc = mcufw_rpc_parse(json, strlen(json), &req);
    check("rpc/parse/get_prop", rc == MCUFW_RPC_OK && strcmp(req.method, "get_prop") == 0 && req.id == 3);
}

/* ---- MQTT payloads ------------------------------------------------------ */
static void run_mqtt(void)
{
    char buf[512];
    int len;

    /* Discovery topic format */
    len = mcufw_mqtt_discovery_topic(0x05eb8034, buf, sizeof(buf));
    check("mqtt/discovery_topic", len > 0 && strstr(buf, "homeassistant/light/05eb8034/config") != NULL);

    /* State topic format */
    len = mcufw_mqtt_state_topic(0x05eb8034, buf, sizeof(buf));
    check("mqtt/state_topic", len > 0 && strstr(buf, "yeelight/05eb8034/state") != NULL);

    /* Command topic format */
    len = mcufw_mqtt_cmd_topic(0x05eb8034, buf, sizeof(buf));
    check("mqtt/cmd_topic", len > 0 && strstr(buf, "yeelight/05eb8034/set") != NULL);

    /* Discovery payload contains required fields */
    len = mcufw_mqtt_discovery_light(0x05eb8034, buf, sizeof(buf));
    check("mqtt/discovery_payload", len > 0 &&
          strstr(buf, "\"brightness\":true") != NULL &&
          strstr(buf, "\"color_temp\":true") != NULL &&
          strstr(buf, "\"unique_id\":\"yeelight_05eb8034\"") != NULL);

    /* State payload serialization */
    len = mcufw_mqtt_state_payload(1, 80, 4000, 120, 50, buf, sizeof(buf));
    check("mqtt/state_payload", len > 0 &&
          strstr(buf, "\"power\":\"on\"") != NULL &&
          strstr(buf, "\"bright\":80") != NULL &&
          strstr(buf, "\"ct\":4000") != NULL);
}

/* ---- Color Flow --------------------------------------------------------- */
static void run_cf(void)
{
    mcufw_cf_params_t params;
    int rc;

    /* Basic parse succeeds */
    rc = mcufw_cf_parse("[3,1,\"500,1,2700,100\"]", 24, &params);
    check("cf/parse/basic", rc == 0 && params.expr_len > 0);

    /* Start accepts valid params (no-op in 3.5) */
    params.action = MCUFW_CF_ACTION_ON;
    rc = mcufw_cf_start(&params);
    check("cf/start/accept", rc == 0);

    /* Invalid action rejected */
    params.action = 99;
    rc = mcufw_cf_start(&params);
    check("cf/start/reject_bad_action", rc < 0);

    /* Stop is safe no-op */
    mcufw_cf_stop();
    check("cf/stop/noop", 1);
}

int main(void)
{
    printf("=== mcufw phase 3.5 host test ===\n");

    printf("\n[crypto + miIO selftest]\n");
    run_selftest();

    printf("\n[LAN-RPC whitelist]\n");
    run_rpc_whitelist();

    printf("\n[LAN-RPC dispatch]\n");
    run_rpc_dispatch();

    printf("\n[MQTT payloads]\n");
    run_mqtt();

    printf("\n[Color Flow]\n");
    run_cf();

    printf("\n=== TOTAL: %u PASS, %u FAIL ===\n", total_pass, total_fail);
    return total_fail == 0 ? 0 : 1;
}
