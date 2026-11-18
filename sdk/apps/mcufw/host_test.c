/*
 * host_test.c -- mcufw phase 3.6 host test suite.
 *
 * Covers:
 *   - Crypto + miIO selftest (from phase 3.4)
 *   - LAN-RPC whitelist enforcement (blocked vs allowed methods)
 *   - MQTT discovery/state payload serialization
 *   - Color Flow parameter parsing
 *   - Lamp state model: range clamping, toggle, name sanitising (3.6)
 *   - PWM backend seam: duty word, recorded mode/calls (3.6)
 *   - RPC/miIO dispatch mutating that state, response carries the new value
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
#include "mcufw_state.h"
#include "mcufw_pwm.h"
#include "mcufw_miio.h"
#include "mcufw_crypto.h"

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

    /* Allowed method -> ok response (params mandatory since 3.6) */
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_power");
    strcpy(req.params, "[\"on\"]");
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

/* ---- lamp state model (3.6) -------------------------------------------- */
static void run_state_model(void)
{
    mcufw_state_t *st = mcufw_state();
    char long_name[48];

    mcufw_state_init(st);
    check("state/init/defaults",
          st->power == 0 && st->bright == 100 && st->ct == 4000 &&
          st->color_mode == MCUFW_COLOR_MODE_CT && st->name[0] == '\0');

    mcufw_state_set_bright(st, 150);
    check("state/clamp/bright_150_to_100", st->bright == 100);
    mcufw_state_set_bright(st, 0);
    check("state/clamp/bright_0_to_1", st->bright == 1);
    mcufw_state_set_bright(st, 42);
    check("state/set/bright_42", st->bright == 42);

    mcufw_state_set_ct(st, 1000);
    check("state/clamp/ct_1000_to_1700", st->ct == 1700);
    mcufw_state_set_ct(st, 9000);
    check("state/clamp/ct_9000_to_6500", st->ct == 6500);
    mcufw_state_set_ct(st, 3000);
    check("state/set/ct_3000", st->ct == 3000);

    mcufw_state_set_hsv(st, 400, 150, 80);
    check("state/clamp/hsv_ranges",
          st->hue == 359 && st->sat == 100 && st->bright == 80);
    check("state/hsv/sets_color_mode", st->color_mode == MCUFW_COLOR_MODE_HSV);

    mcufw_state_set_ct(st, 2700);
    check("state/ct/resets_color_mode", st->color_mode == MCUFW_COLOR_MODE_CT);

    mcufw_state_set_power(st, 1);
    check("state/power/on", st->power == 1);
    mcufw_state_toggle(st);
    check("state/toggle/on_to_off", st->power == 0);
    mcufw_state_toggle(st);
    check("state/toggle/off_to_on", st->power == 1);

    mcufw_state_set_name(st, "Kitchen");
    check("state/name/copy", strcmp(st->name, "Kitchen") == 0);

    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    mcufw_state_set_name(st, long_name);
    check("state/name/truncated_to_32", strlen(st->name) == MCUFW_NAME_MAX);

    mcufw_state_set_name(st, "a\"b");
    check("state/name/sanitised", strcmp(st->name, "a_b") == 0);

    mcufw_state_set_cf(st, 1);
    check("state/cf/active", st->cf_active == 1);
    mcufw_state_set_cf(st, 0);
    check("state/cf/stopped", st->cf_active == 0);

    mcufw_state_set_default(st, 1);
    check("state/default/flag", st->is_default == 1);

    check("state/mutations/counted", st->mutations >= 15);
    check("state/clamp_helpers/direct",
          mcufw_state_clamp_bright(150) == 100 &&
          mcufw_state_clamp_ct(1700) == 1700 &&
          mcufw_state_clamp_hue(0) == 0 &&
          mcufw_state_clamp_sat(101) == 100);
}

/* ---- PWM backend seam (3.6) -------------------------------------------- */
static void run_pwm_backend(void)
{
    mcufw_state_t *st = mcufw_state();
    const mcufw_pwm_stats_t *pwm = mcufw_pwm_stats();
    uint32_t set_before, hsv_before;

    check("pwm/duty/0", mcufw_pwm_duty(0) == 0);
    check("pwm/duty/100_full_scale", mcufw_pwm_duty(100) == MCUFW_PWM_PERIOD);
    check("pwm/duty/50_half", mcufw_pwm_duty(50) == (MCUFW_PWM_PERIOD / 2u));

    mcufw_state_init(st);
    check("pwm/init/off_recorded",
          pwm->mode == MCUFW_PWM_MODE_OFF && pwm->bright == 0 &&
          pwm->set_calls == 1);

    set_before = pwm->set_calls;
    mcufw_state_set_power(st, 1);
    mcufw_state_set_bright(st, 60);
    check("pwm/set/mode_ct_after_bright",
          pwm->mode == MCUFW_PWM_MODE_CT && pwm->bright == 60 &&
          pwm->ct == st->ct);
    check("pwm/set/calls_incremented", pwm->set_calls > set_before);
    check("pwm/set/log_has_duty", strstr(pwm->last_log, "duty=") != NULL);

    hsv_before = pwm->hsv_calls;
    mcufw_state_set_hsv(st, 200, 60, 80);
    check("pwm/hsv/mode_and_values",
          pwm->mode == MCUFW_PWM_MODE_HSV && pwm->hue == 200 &&
          pwm->sat == 60 && pwm->val == 80);
    check("pwm/hsv/calls_incremented", pwm->hsv_calls == hsv_before + 1u);

    mcufw_state_set_power(st, 0);
    check("pwm/off/zeroes_bright",
          pwm->mode == MCUFW_PWM_MODE_OFF && pwm->bright == 0);
}

/* ---- RPC dispatch mutating the state ----------------------------------- */
static void run_rpc_state(void)
{
    char out[256];
    mcufw_rpc_request_t req;
    mcufw_state_t *st = mcufw_state();
    int rc;

    mcufw_state_init(st);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_bright");
    strcpy(req.params, "[150,\"smooth\",500]");
    req.id = 10;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_bright_clamped",
          rc > 0 && st->bright == 100 && strstr(out, "\"ok\",100") != NULL);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_ct_abx");
    strcpy(req.params, "[1000,3]");
    req.id = 11;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_ct_clamped",
          rc > 0 && st->ct == 1700 && strstr(out, "\"ok\",1700") != NULL);

    mcufw_state_set_power(st, 1);
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "toggle");
    req.id = 12;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/toggle_flips_power",
          rc > 0 && st->power == 0 && strstr(out, "\"ok\",0") != NULL);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_hsv");
    strcpy(req.params, "[200,60,80]");
    req.id = 13;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_hsv_response",
          rc > 0 && st->hue == 200 && st->sat == 60 && st->bright == 80 &&
          strstr(out, "\"ok\",200,60,80") != NULL);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_name");
    strcpy(req.params, "[\"Kitchen\"]");
    req.id = 14;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_name_echoes",
          rc > 0 && strcmp(st->name, "Kitchen") == 0 &&
          strstr(out, "\"ok\",\"Kitchen\"") != NULL);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "start_cf");
    strcpy(req.params, "[3,1,\"500,1,2700,100\"]");
    req.id = 15;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/start_cf_sets_active", rc > 0 && st->cf_active == 1);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_power");
    strcpy(req.params, "[\"on\"]");
    req.id = 16;
    req.have_id = 1;
    (void)mcufw_rpc_dispatch(&req, out, sizeof(out));
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_bright");
    strcpy(req.params, "[42]");
    req.id = 17;
    req.have_id = 1;
    (void)mcufw_rpc_dispatch(&req, out, sizeof(out));
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "get_prop");
    strcpy(req.params, "[\"power\",\"bright\",\"ct\"]");
    req.id = 18;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/get_prop_readback",
          rc > 0 && strstr(out, "\"on\",42,1700") != NULL);

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_power");
    strcpy(req.params, "[\"maybe\"]");
    req.id = 19;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_power_bad_param",
          rc > 0 && strstr(out, "\"code\":3") != NULL);
}

/* ---- miIO dispatch (UDP 54321) mutating the same state ------------------ */
static void build_miio_request(mcufw_miio_ctx_t *ctx, const char *json,
                               uint8_t *out, size_t *out_len)
{
    uint8_t key[MCUFW_AES_BLOCK], iv[MCUFW_AES_BLOCK], digest[MCUFW_MD5_LEN];
    size_t jlen = strlen(json);
    size_t plen;
    uint16_t total;

    mcufw_md5(ctx->token, MCUFW_MIIO_TOKEN_LEN, key);
    mcufw_md5_2(key, MCUFW_AES_BLOCK, ctx->token, MCUFW_MIIO_TOKEN_LEN, iv);

    memset(out, 0, MCUFW_MIIO_HDR_LEN);
    out[0] = (uint8_t)(MCUFW_MIIO_MAGIC & 0xffu);
    out[1] = (uint8_t)(MCUFW_MIIO_MAGIC >> 8);
    out[8] = (uint8_t)(ctx->did & 0xffu);
    out[9] = (uint8_t)((ctx->did >> 8) & 0xffu);
    out[10] = (uint8_t)((ctx->did >> 16) & 0xffu);
    out[11] = (uint8_t)((ctx->did >> 24) & 0xffu);

    plen = mcufw_aes128_cbc_encrypt(key, iv, (const uint8_t *)json, jlen,
                                    out + MCUFW_MIIO_HDR_LEN);
    total = (uint16_t)(MCUFW_MIIO_HDR_LEN + plen);
    out[2] = (uint8_t)(total & 0xffu);
    out[3] = (uint8_t)(total >> 8);
    mcufw_md5_2(out, 16u, ctx->token, MCUFW_MIIO_TOKEN_LEN, digest);
    memcpy(out + 16, digest, MCUFW_MD5_LEN);
    *out_len = total;
}

/* Send one miIO method and decrypt the reply into `plain`. */
static int miio_call(mcufw_miio_ctx_t *ctx, const char *json,
                     uint8_t *rsp, size_t rsp_cap, size_t *rsp_len,
                     char *plain, size_t plain_cap)
{
    uint8_t req[512];
    uint8_t key[MCUFW_AES_BLOCK], iv[MCUFW_AES_BLOCK];
    size_t req_len = 0, clen;
    int rc;

    build_miio_request(ctx, json, req, &req_len);
    rc = mcufw_miio_handle(ctx, req, req_len, rsp, rsp_cap, rsp_len);
    if (rc != MCUFW_MIIO_OK || *rsp_len <= MCUFW_MIIO_HDR_LEN) {
        plain[0] = '\0';
        return rc;
    }
    mcufw_md5(ctx->token, MCUFW_MIIO_TOKEN_LEN, key);
    mcufw_md5_2(key, MCUFW_AES_BLOCK, ctx->token, MCUFW_MIIO_TOKEN_LEN, iv);
    clen = *rsp_len - MCUFW_MIIO_HDR_LEN;
    {
        size_t n = mcufw_aes128_cbc_decrypt(key, iv, rsp + MCUFW_MIIO_HDR_LEN,
                                           clen, (uint8_t *)plain);
        if (n >= plain_cap)
            n = plain_cap - 1;
        plain[n] = '\0';
    }
    return rc;
}

static void run_miio_state(void)
{
    mcufw_miio_ctx_t ctx;
    mcufw_state_t *st = mcufw_state();
    uint8_t rsp[1024];
    size_t rsp_len = 0;
    char plain[768];
    uint8_t token[MCUFW_MIIO_TOKEN_LEN];
    int rc;

    /* Pair the channel with a known token so encrypted requests are accepted. */
    mcufw_miio_ctx_init(&ctx, 0x05eb8034u);
    memset(token, 0x11, sizeof(token));
    mcufw_miio_set_token(&ctx, token);

    mcufw_state_init(st);

    rc = miio_call(&ctx, "{\"id\":1,\"method\":\"set_ps\",\"params\":[\"on\"]}",
                   rsp, sizeof(rsp), &rsp_len, plain, sizeof(plain));
    check("miio/set_ps/on",
          rc == MCUFW_MIIO_OK && st->power == 1 && strstr(plain, "\"ok\"") != NULL);

    rc = miio_call(&ctx, "{\"id\":2,\"method\":\"get_ps\"}",
                   rsp, sizeof(rsp), &rsp_len, plain, sizeof(plain));
    check("miio/get_ps/reflects_state",
          rc == MCUFW_MIIO_OK && strstr(plain, "\"power\":\"on\"") != NULL &&
          strstr(plain, "\"bright\":100") != NULL);

    mcufw_state_set_bright(st, 55);
    mcufw_state_set_name(st, "Kitchen");
    rc = miio_call(&ctx, "{\"id\":3,\"method\":\"miIO.info\"}",
                   rsp, sizeof(rsp), &rsp_len, plain, sizeof(plain));
    check("miio/info/live_state",
          rc == MCUFW_MIIO_OK && strstr(plain, "0.6-phase3.6") != NULL &&
          strstr(plain, "\"power\":\"on\"") != NULL &&
          strstr(plain, "\"bright\":55") != NULL &&
          strstr(plain, "\"name\":\"Kitchen\"") != NULL);

    rc = miio_call(&ctx, "{\"id\":4,\"method\":\"set_ps\",\"params\":[\"off\"]}",
                   rsp, sizeof(rsp), &rsp_len, plain, sizeof(plain));
    check("miio/set_ps/off", rc == MCUFW_MIIO_OK && st->power == 0);

    rc = miio_call(&ctx, "{\"id\":5,\"method\":\"set_ps\",\"params\":[\"maybe\"]}",
                   rsp, sizeof(rsp), &rsp_len, plain, sizeof(plain));
    check("miio/set_ps/bad_param",
          rc == MCUFW_MIIO_OK && strstr(plain, "-32602") != NULL);
}

int main(void)
{
    printf("=== mcufw phase 3.6 host test ===\n");

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

    printf("\n[Lamp state model]\n");
    run_state_model();

    printf("\n[PWM backend]\n");
    run_pwm_backend();

    printf("\n[LAN-RPC state mutation]\n");
    run_rpc_state();

    printf("\n[miIO state mutation]\n");
    run_miio_state();

    printf("\n=== TOTAL: %u PASS, %u FAIL ===\n", total_pass, total_fail);
    return total_fail == 0 ? 0 : 1;
}
