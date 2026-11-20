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
 *   - PWM register writes: exact GPT0 CMR0/USER_REQ words per command (3.7)
 *   - RPC/miIO dispatch mutating that state, response carries the new value
 *   - Runtime owner (3.8): bounded boot sweep, single writer, idle invariant,
 *     network pumps driven by the service loop
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
#include "mcufw_pwm_hal.h"
#include "mcufw_regs.h"
#include "mcufw_miio.h"
#include "mcufw_crypto.h"
#include "mcufw_net.h"
#include "mcufw_net_host.h"
#include "mcufw_runtime.h"
#include "mcufw_config.h"
#include "mcufw_config_hal.h"
#include "mcufw_rollback.h"
#include "mcufw_devcfg.h"
#include "mcufw_ota.h"
#include "mcufw_http_portal.h"

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
    check("pwm/set/log_has_duty", strstr(pwm->last_log, "pwm ct b=") != NULL);

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

/* ---- PWM register writes (3.7) ----------------------------------------- */
/* Read back one channel's CMR0 from the mocked GPT0 window. */
static uint32_t cmr0(unsigned ch)
{
    return mcufw_pwm_hal_stub_read(GPT_CHAN(ch) + GPT_CH_CMR0);
}

static void run_pwm_register(void)
{
    mcufw_state_t *st = mcufw_state();
    const mcufw_pwm_stats_t *pwm = mcufw_pwm_stats();

    mcufw_pwm_hal_stub_reset();
    mcufw_pwm_init();

    check("pwm_register/backend_is_stub",
          strcmp(mcufw_pwm_hal_name(), "stub-array") == 0);
    check("pwm_register/init_writes_nothing", mcufw_pwm_hal_stub_writes() == 0u);

    /* 100 % -> duty 0xC350 -> inverted CMR0 0x0000 on both white channels,
     * colour pair dark (CMR0 = period). */
    mcufw_pwm_set(100, 4000);
    check("pwm_register/bright100_ch0_cmr0_0000",
          cmr0(MCUFW_PWM_CH_G) == 0x0000u);
    check("pwm_register/bright100_ch1_cmr0_0000",
          cmr0(MCUFW_PWM_CH_G) == 0x0000u);
    check("pwm_register/bright100_color_pair_dark",
          cmr0(MCUFW_PWM_CH_R) == 0x1637u &&
          cmr0(MCUFW_PWM_CH_B) == 0xa261u);
    check("pwm_register/bright100_level_full_scale",
          pwm->ch_level[3] == 45882u && pwm->ch_level[1] == MCUFW_PWM_PERIOD);

    /* 50 % -> duty 0x61A8 -> CMR0 0xC350 - 0x61A8 = 0x61A8. */
    mcufw_pwm_set(50, 4000);
    check("pwm_register/bright50_ch0_cmr0_61a8", cmr0(MCUFW_PWM_CH_G) == 0x61a8u);
    check("pwm_register/bright50_ch1_cmr0_61a8", cmr0(MCUFW_PWM_CH_W) == 0x69b3u);
    /* Every CMR0 write must be followed by its own CHn_CMR_UPDT latch, so
     * all four channels really take the new match value. */
    check("pwm_register/all_channels_latched_after_bright50",
          mcufw_pwm_hal_stub_latched_mask() == 0x33u);

    /* 1 % -> duty 500 counts (0x01F4) -> CMR0 0xC350 - 0x01F4 = 0xC15C.
     * (Period/100 = 500 is integer, so no rounding occurs; the ticket-level
     * guess "0xC347" does not reproduce and is superseded by this value.) */
    mcufw_pwm_set(1, 4000);
    check("pwm_register/bright1_ch0_cmr0_c15c", cmr0(MCUFW_PWM_CH_G) == 0xc15cu);
    check("pwm_register/bright1_ch1_cmr0_c15c", cmr0(MCUFW_PWM_CH_W) == 0xc186u);

    /* HSV v=80 -> duty 40000 (0x9C40) -> CMR0 10000 (0x2710) on CH4/CH5,
     * white pair dark. */
    mcufw_pwm_hsv(200, 60, 80);
    check("pwm_register/hsv80_color_ch4_cmr0_2710", cmr0(MCUFW_PWM_CH_B) == 0x2710u);
    check("pwm_register/hsv80_color_ch5_cmr0_2710", cmr0(MCUFW_PWM_CH_W) == 0x9385u);
    check("pwm_register/hsv_white_pair_dark",
          cmr0(MCUFW_PWM_CH_R) == 0x4bd4u && cmr0(MCUFW_PWM_CH_G) == 0x3352u);
    check("pwm_register/hsv_latch_on_ch5",
          mcufw_pwm_hal_stub_read(GPT_USER_REQ) == GPT_CMR_UPDT(5));

    /* Power off: all four channels to level 0 -> CMR0 = 0xC350 (inverted
     * polarity: writing 0 would mean FULL brightness). */
    mcufw_pwm_set(0, 4000);
    check("pwm_register/off_all_channels_level0",
          cmr0(0) == MCUFW_PWM_PERIOD && cmr0(1) == MCUFW_PWM_PERIOD &&
          cmr0(4) == MCUFW_PWM_PERIOD && cmr0(5) == MCUFW_PWM_PERIOD);
    check("pwm_register/off_levels_zero",
          pwm->ch_level[3] == 0u && pwm->ch_level[0] == 0u &&
          pwm->powered == 0u);
    check("pwm_register/off_remembers_last_command",
          pwm->restore_ch[0] == 30588u && pwm->restore_ch[3] == 12235u &&
          pwm->restore_mode == MCUFW_PWM_MODE_HSV &&
          pwm->restore_bright == 80u);

    /* Restore after off brings the colour pair back (same duty words). */
    mcufw_pwm_restore();
    check("pwm_register/restore_color_cmr0_2710",
          cmr0(MCUFW_PWM_CH_B) == 0x2710u && cmr0(MCUFW_PWM_CH_W) == 0x9385u && pwm->powered == 1u);
    check("pwm_register/restore_keeps_white_dark",
          cmr0(MCUFW_PWM_CH_R) == 0x4bd4u && cmr0(MCUFW_PWM_CH_G) == 0x3352u);

    /* End-to-end through the state model (the path the network front-ends
     * use): power off must zero all channels, power on must restore the
     * previous brightness. */
    mcufw_pwm_hal_stub_reset();
    mcufw_state_init(st);
    check("pwm_register/state_init_all_dark",
          cmr0(0) == MCUFW_PWM_PERIOD && cmr0(1) == MCUFW_PWM_PERIOD &&
          cmr0(4) == MCUFW_PWM_PERIOD && cmr0(5) == MCUFW_PWM_PERIOD);

    mcufw_state_set_power(st, 1);
    mcufw_state_set_bright(st, 50);
    check("pwm_register/state_bright50_cmr0_61a8",
          cmr0(MCUFW_PWM_CH_G) == 0x61a8u && cmr0(MCUFW_PWM_CH_W) == 0x69b3u &&
          cmr0(MCUFW_PWM_CH_B) == 0xb2d9u);

    mcufw_state_set_power(st, 0);
    check("pwm_register/state_power_off_dark",
          cmr0(0) == MCUFW_PWM_PERIOD && cmr0(1) == MCUFW_PWM_PERIOD &&
          cmr0(4) == MCUFW_PWM_PERIOD && cmr0(5) == MCUFW_PWM_PERIOD);

    mcufw_state_set_power(st, 1);
    check("pwm_register/state_power_on_restores_61a8",
          cmr0(MCUFW_PWM_CH_G) == 0x61a8u && cmr0(MCUFW_PWM_CH_W) == 0x69b3u &&
          cmr0(MCUFW_PWM_CH_B) == 0xb2d9u && cmr0(MCUFW_PWM_CH_R) == 0x6cc4u);

    check("pwm_register/no_out_of_window_access",
          mcufw_pwm_hal_stub_bad_offset() == 0u);
    check("pwm_register/no_latch_mismatch",
          mcufw_pwm_hal_stub_latch_errors() == 0u);
    check("pwm_register/write_counters_advance",
          mcufw_pwm_hal_stub_writes() > 0u && pwm->reg_writes > 0u);
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


    /* Power on first so set_rgb apply() drives channels (Review R3 fix) */
    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_power");
    strcpy(req.params, "[\"on\"]");
    req.id = 99;
    req.have_id = 1;
    mcufw_rpc_dispatch(&req, out, sizeof(out));

    memset(&req, 0, sizeof(req));
    strcpy(req.method, "set_rgb");
    /* Yeelight RGB integer: (128<<16)|(64<<8)|32 = 8405024 */
    strcpy(req.params, "[8405024]");
    req.id = 15;
    req.have_id = 1;
    rc = mcufw_rpc_dispatch(&req, out, sizeof(out));
    check("rpc/state/set_rgb_response",
          rc > 0 && strstr(out, "\"ok\",8405024") != NULL);
    /* Verify PWM backend received RGB mode */
    const mcufw_pwm_stats_t *pst = mcufw_pwm_stats();
    check("rpc/state/set_rgb_pwm_mode", pst->mode == MCUFW_PWM_MODE_RGB);
    check("rpc/state/set_rgb_channels", pst->ch_level[0] > 0 && pst->ch_level[1] > 0 && pst->ch_level[2] > 0);

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
          rc == MCUFW_MIIO_OK && strstr(plain, "0.8-phase4b.") != NULL &&
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

/* ---- runtime owner / single-writer service loop (phase 3.8) ------------- */
#define DID_HOST 0x05eb8034u

static int      g_host_out;
static int      g_host_button;
static unsigned g_host_status_calls;
static char     g_host_status[160];

static int host_out_read(void) { return g_host_out; }
static int host_button_read(void) { return g_host_button; }
static void host_status_write(const char *line)
{
    g_host_status_calls++;
    snprintf(g_host_status, sizeof(g_host_status), "%s", line);
}

static void run_runtime_owner(void)
{
    static const mcufw_runtime_io_t io = {
        host_out_read, host_button_read, host_status_write
    };
    const mcufw_runtime_stats_t *rt;
    const mcufw_net_stats_t *net;
    const mcufw_net_host_stats_t *hst;
    mcufw_state_t *st = mcufw_state();
    uint32_t cmr0_cmd;
    uint32_t reg_after_cmd;
    unsigned i;
    int rc;

    /* fresh world: pwm backend forgets everything, host transport plugged in */
    mcufw_state_init(st);
    mcufw_pwm_init();
    mcufw_pwm_stats_reset();
    mcufw_net_host_reset();
    rc = mcufw_net_init(mcufw_net_host_transport(), DID_HOST);

    g_host_out = 1;
    g_host_button = 0;
    g_host_status_calls = 0;
    g_host_status[0] = '\0';
    mcufw_runtime_init(&io);

    rt = mcufw_runtime_stats();
    net = mcufw_net_stats();
    hst = mcufw_net_host_stats();

    /* -- 1. transport really plugged in, loop samples the board ----------- */
    for (i = 0; i < 3; i++)
        mcufw_runtime_tick();
    check("runtime_owner/net_init", rc == 0);
    check("runtime_owner/board_io_per_tick",
          rt->ticks == 3u && rt->out_reads == 3u && rt->button_reads == 3u &&
          rt->last_out == 1u && rt->last_button == 0);

    /* -- 2. both network pumps issued once per tick ----------------------- */
    check("runtime_owner/pumps_per_tick",
          net->udp_pumps == 3u && net->tcp_pumps == 3u &&
          rt->net_udp_pumps == net->udp_pumps &&
          rt->net_tcp_pumps == net->tcp_pumps);

    /* -- 3. a state command lands in GPT0 CMR0 (bright 40 -> 0x7530) ------ */
    mcufw_state_set_power(st, 1);
    mcufw_state_set_bright(st, 40);
    cmr0_cmd = mcufw_pwm_cmr0(MCUFW_PWM_CH_W);
    reg_after_cmd = mcufw_pwm_stats()->reg_writes;
    check("runtime_owner/state_path_writes",
          cmr0_cmd == 0x7ba0u && mcufw_pwm_duty(40) == 20000u &&
          mcufw_pwm_stats()->ch_level[3] == 18352u && reg_after_cmd > 0u);

    /* -- 4. >= 3 loop ticks neither rewrite nor lose that value ----------- */
    {
        uint32_t idle_before;
        uint32_t wt_before;

        mcufw_runtime_tick();       /* the tick that consumes the command  */
        idle_before = rt->idle_ticks;
        wt_before = rt->write_ticks;
        for (i = 0; i < 3; i++)
            mcufw_runtime_tick();
        check("runtime_owner/cmr0_survives_ticks",
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == 0x7ba0u &&
              mcufw_pwm_stats()->reg_writes == reg_after_cmd);
        check("runtime_owner/idle_tick_zero_writes",
              rt->idle_ticks == idle_before + 3u && rt->write_ticks == wt_before &&
              rt->last_tick_writes == 0u);
    }

    /* -- 5. the dirty flag is consumed exactly once -----------------------
     * Note the write itself happens in the command path (state -> pwm),
     * so reg_writes rises on the API call and the tick only consumes the
     * flag: a tick never re-programs on its own. */
    {
        uint32_t wt_before = rt->write_ticks;
        uint32_t applied_before = mcufw_pwm_stats()->service_applied;
        uint32_t rw_before = mcufw_pwm_stats()->reg_writes;
        uint32_t rw_cmd;

        mcufw_state_set_bright(st, 55);
        rw_cmd = mcufw_pwm_stats()->reg_writes;
        mcufw_runtime_tick();                    /* consumes the change   */
        {
            int consumed = (rt->write_ticks == wt_before + 1u &&
                            mcufw_pwm_stats()->service_applied == applied_before + 1u);

            mcufw_runtime_tick();                /* nothing left to do    */
            check("runtime_owner/dirty_consumed_once",
                  rw_cmd > rw_before && consumed &&
                  rt->write_ticks == wt_before + 1u &&
                  rt->last_tick_writes == 0u);
        }
    }

    /* -- 6. a UDP datagram is read by the loop; plain JSON is not a miIO
     *       frame, so the miIO layer rejects it and the lamp stays as it
     *       was.  What this proves: the pump moves real bytes per tick and
     *       a hostile/garbage datagram cannot touch GPT0. */
    {
        static const char req[] = "{\"id\":9,\"method\":\"set_ps\",\"params\":[\"off\"]}";
        uint32_t cmr_before = mcufw_pwm_cmr0(MCUFW_PWM_CH_W);

        mcufw_net_host_push_udp((const uint8_t *)req, sizeof(req) - 1u);
        mcufw_runtime_tick();
        check("runtime_owner/udp_read_rejected",
              net->udp_datagrams == 1u && net->udp_dropped == 1u &&
              st->power == 1 &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == cmr_before);
    }

    /* -- 7. TCP 55443 request through the loop answers with the state ----- */
    {
        static const char line[] = "{\"id\":10,\"method\":\"get_prop\",\"params\":[\"bright\"]}";

        mcufw_net_host_push_line(line, sizeof(line) - 1u);
        mcufw_runtime_tick();
        check("runtime_owner/tcp_dispatch",
              net->tcp_requests == 1u && hst->tcp_sends == 1u &&
              hst->tcp_send_bytes > 0u &&
              strstr(mcufw_net_host_last_tcp_tx(), "55") != NULL);
    }

    /* -- 8. the periodic status line reports the true CMR0 word ----------- */
    while (rt->ticks % MCUFW_RUNTIME_STATUS_EVERY != 0u)
        mcufw_runtime_tick();
    check("runtime_owner/status_line",
          rt->status_lines >= 1u && g_host_status_calls == rt->status_lines &&
          strstr(g_host_status, "cmr0 0x") != NULL &&
          strstr(g_host_status, "udp 1") != NULL &&
          strstr(g_host_status, "tcp 1") != NULL);

    printf("  runtime_owner: ticks=%u udp_pumps=%u tcp_pumps=%u "
           "udp_datagrams=%u tcp_requests=%u idle=%u write_ticks=%u "
           "loop_reg_writes=%u (writes issued BY a tick; the state path "
           "programs CMR0 at command time)\n",
           rt->ticks, net->udp_pumps, net->tcp_pumps, net->udp_datagrams,
           net->tcp_requests, rt->idle_ticks, rt->write_ticks,
           rt->reg_writes_total);
}

/* ---- persistent config / power-on behaviour (phase 4) ------------------- */
static void run_config_persist(void)
{
    mcufw_state_t st;
    mcufw_state_t loaded;
    const mcufw_config_stats_t *cfg;
    uint8_t block[MCUFW_CONFIG_SIZE];
    uint8_t *raw;
    unsigned i;
    int rc;

    /* -- 1. round trip: every field comes back bit-exact ------------------ */
    mcufw_config_init();
    mcufw_config_hal_stub_reset();
    mcufw_state_init(&st);
    st.power = 1u;
    st.bright = 40u;
    st.ct = 2700u;
    st.hue = 200u;
    st.sat = 60u;
    st.color_mode = MCUFW_COLOR_MODE_HSV;
    strcpy(st.name, "desk-lamp");
    rc = mcufw_config_save(&st);
    cfg = mcufw_config_stats();
    check("config_persist/save_ok",
          rc == MCUFW_CFG_OK && cfg->save_ok == 1u &&
          mcufw_config_hal_stub_writes() == 1u &&
          mcufw_config_hal_stub_valid() == 1);

    mcufw_state_init(&loaded);
    rc = mcufw_config_load(&loaded);
    cfg = mcufw_config_stats();
    check("config_persist/roundtrip_values",
          rc == MCUFW_CFG_OK && cfg->load_ok == 1u &&
          loaded.power == 1u && loaded.bright == 40u && loaded.ct == 2700u &&
          loaded.hue == 200u && loaded.sat == 60u &&
          loaded.color_mode == MCUFW_COLOR_MODE_HSV &&
          strcmp(loaded.name, "desk-lamp") == 0);
    check("config_persist/block_size_48",
          mcufw_config_encode(&st, block, sizeof(block)) == 48u &&
          memcmp(block, mcufw_config_hal_stub_bytes(), 48u) == 0);

    /* -- 2. corrupt CRC -> defaults, ZERO foreign fields, ONE log line ---- */
    raw = mcufw_config_hal_stub_bytes();
    raw[MCUFW_CONFIG_CRC_OFF] ^= 0x01u;
    mcufw_state_init(&loaded);
    rc = mcufw_config_load(&loaded);
    cfg = mcufw_config_stats();
    check("config_persist/crc_defaults",
          rc == MCUFW_CFG_ERR_CRC && cfg->load_defaults == 1u &&
          cfg->foreign_fields == 0u &&
          loaded.power == 0u && loaded.bright == 100u && loaded.ct == 4000u &&
          loaded.hue == 0u && loaded.sat == 0u &&
          loaded.color_mode == MCUFW_COLOR_MODE_CT && loaded.name[0] == '\0');
    check("config_persist/crc_one_log",
          cfg->logs == 1u && strstr(cfg->last_log, "bad-crc") != NULL);

    /* -- 3. broken magic -> defaults -------------------------------------- */
    mcufw_config_init();
    mcufw_config_hal_stub_reset();
    mcufw_config_save(&st);
    raw = mcufw_config_hal_stub_bytes();
    raw[0] ^= 0xffu;
    mcufw_state_init(&loaded);
    rc = mcufw_config_load(&loaded);
    cfg = mcufw_config_stats();
    check("config_persist/magic_defaults",
          rc == MCUFW_CFG_ERR_MAGIC && cfg->load_defaults == 1u &&
          cfg->foreign_fields == 0u && loaded.power == 0u &&
          loaded.bright == 100u && loaded.ct == 4000u &&
          cfg->logs == 1u && strstr(cfg->last_log, "bad-magic") != NULL);

    /* -- 4. version mismatch -> defaults ---------------------------------- */
    mcufw_config_init();
    mcufw_config_hal_stub_reset();
    mcufw_config_save(&st);
    raw = mcufw_config_hal_stub_bytes();
    raw[4] = (uint8_t)(MCUFW_CONFIG_VERSION + 1u);
    mcufw_state_init(&loaded);
    rc = mcufw_config_load(&loaded);
    cfg = mcufw_config_stats();
    check("config_persist/version_defaults",
          rc == MCUFW_CFG_ERR_VERSION && cfg->load_defaults == 1u &&
          cfg->foreign_fields == 0u && loaded.power == 0u &&
          loaded.bright == 100u && loaded.ct == 4000u &&
          cfg->logs == 1u && strstr(cfg->last_log, "bad-version") != NULL);

    /* -- 5. boot with power=0: all four channels dark (CMR0 = 0xC350) ----- */
    {
        static const mcufw_runtime_io_t io = {
            host_out_read, host_button_read, host_status_write
        };
        const mcufw_runtime_stats_t *rt;
        mcufw_state_t *gst;
        uint32_t writes_before;
        uint32_t cfg_writes_before;

        mcufw_config_init();
        mcufw_config_hal_stub_reset();
        mcufw_state_init(&st);
        st.power = 0u;
        st.bright = 40u;
        mcufw_config_save(&st);

        mcufw_pwm_hal_stub_reset();
        gst = mcufw_state();
        rc = mcufw_config_boot(gst);
        mcufw_pwm_invalidate();
        mcufw_state_apply(gst);
        check("config_persist/boot_power0_dark",
              rc == MCUFW_CFG_OK && gst->power == 0u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == 0xc350u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == 0xc350u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_R) == 0xc350u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_G) == 0xc350u);

        /* -- 6. idle ticks after a load perform ZERO config writes -------- */
        mcufw_net_host_reset();
        mcufw_net_init(mcufw_net_host_transport(), DID_HOST);
        mcufw_runtime_init(&io);
        rt = mcufw_runtime_stats();
        writes_before = mcufw_config_hal_stub_writes();
        cfg_writes_before = mcufw_config_stats()->hal_writes;
        for (i = 0; i < 64u; i++)
            mcufw_runtime_tick();
        cfg = mcufw_config_stats();
        check("config_persist/idle_zero_writes",
              rt->ticks == 64u && rt->config_saves == 0u &&
              mcufw_config_hal_stub_writes() - writes_before == 0u &&
              cfg->hal_writes - cfg_writes_before == 0u);

        /* -- 7. one mutation -> exactly one save on the next tick --------- */
        mcufw_state_set_bright(gst, 55);
        mcufw_runtime_tick();
        cfg = mcufw_config_stats();
        check("config_persist/mutation_one_save",
              rt->config_saves == 1u &&
              cfg->hal_writes - cfg_writes_before == 1u &&
              mcufw_config_hal_stub_writes() == writes_before + 1u);
        for (i = 0; i < 16u; i++)
            mcufw_runtime_tick();
        check("config_persist/no_rewrite_after_save",
              rt->config_saves == 1u &&
              mcufw_config_hal_stub_writes() == writes_before + 1u);
    }

    /* -- 8. boot with power=1 bright=40: CMR0 = 0x7530 (duty 20000) ------- */
    mcufw_config_init();
    mcufw_config_hal_stub_reset();
    mcufw_state_init(&st);
    st.power = 1u;
    st.bright = 40u;
    st.ct = 4000u;
    st.color_mode = MCUFW_COLOR_MODE_CT;
    mcufw_config_save(&st);
    mcufw_pwm_hal_stub_reset();
    {
        mcufw_state_t *gst = mcufw_state();

        rc = mcufw_config_boot(gst);
        mcufw_pwm_invalidate();
        mcufw_state_apply(gst);
        check("config_persist/boot_power1_duty",
              rc == MCUFW_CFG_OK && gst->power == 1u && gst->bright == 40u &&
              mcufw_pwm_duty(40u) == 20000u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == 0x7ba0u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_W) == 0x7ba0u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_R) == 0x7e13u &&
              mcufw_pwm_cmr0(MCUFW_PWM_CH_B) == 0xb624u);
    }

    /* -- 9. name sanitising survives the persist path --------------------- */
    mcufw_config_init();
    mcufw_config_hal_stub_reset();
    mcufw_state_init(&st);
    mcufw_state_set_name(&st, "a\"b\\c\td");
    mcufw_config_save(&st);
    mcufw_state_init(&loaded);
    rc = mcufw_config_load(&loaded);
    check("config_persist/name_sanitised",
          rc == MCUFW_CFG_OK && strcmp(loaded.name, "a_b_c_d") == 0);

    /* -- 10. CRC variant is the MRVL container one (init 0, no final xor).
     * Independently computed in Python: crc32(b"123456789", init=0,
     * poly=0xEDB88320 reflected, no final xor) = 0x2dfd2d88.
     * This differs from standard zlib.crc32 (0xCBF43926) by design. */
    {
        static const uint8_t vec[] = "123456789";
        uint32_t c = mcufw_config_crc32(vec, 9u);
        check("config_persist/crc_variant_mrvl",
              c == 0x2dfd2d88u && c != 0xcbf43926u);
    }

    /* -- 11. identical save is skipped (no flash wear) -------------------- */
    {
        uint32_t w = mcufw_config_hal_stub_writes();

        rc = mcufw_config_save(&st);
        cfg = mcufw_config_stats();
        check("config_persist/identical_skipped",
              rc == MCUFW_CFG_OK && cfg->save_skipped == 1u &&
              mcufw_config_hal_stub_writes() == w);
    }

    cfg = mcufw_config_stats();
    printf("  config_persist: loads=%u ok=%u defaults=%u saves=%u ok=%u "
           "skipped=%u hal_writes=%u foreign_fields=%u logs=%u "
           "backend_writes=%u\n",
           cfg->loads, cfg->load_ok, cfg->load_defaults, cfg->saves,
           cfg->save_ok, cfg->save_skipped, cfg->hal_writes,
           cfg->foreign_fields, cfg->logs, mcufw_config_hal_stub_writes());
}

/* ---- Phase 4b.1: Rollback early-boot check ----------------------------- */
extern void mcufw_rollback_test_reset_state(void);
extern int  mcufw_rollback_test_was_passive_activated(void);
extern int  mcufw_rollback_test_was_reset_called(void);
extern void mcufw_rollback_test_force_read_error(int enable);
extern void mcufw_rollback_test_force_write_error(int enable);
extern void mcufw_rollback_test_force_activate_error(int enable);

static void run_rollback(void)
{
    for (unsigned i = 0; i < 4; i++) {
        mcufw_rollback_test_reset_state();
        mcufw_rollback_set_counter(i);
        mcufw_rollback_result_t rb = mcufw_rollback_early_boot_check();
        char label[64];
        snprintf(label, sizeof(label), "rollback/boot_%u_ok", i);
        check(label, rb == MCUFW_RB_OK && mcufw_rollback_read_counter() == i + 1);
    }
    mcufw_rollback_test_reset_state();
    mcufw_rollback_set_counter(4);
    mcufw_rollback_result_t rb = mcufw_rollback_early_boot_check();
    check("rollback/threshold_swap", mcufw_rollback_test_was_passive_activated());
    check("rollback/threshold_counter_reset", mcufw_rollback_read_counter() == 0);
    mcufw_rollback_test_reset_state();
    mcufw_rollback_set_counter(3);
    mcufw_rollback_confirm_good();
    check("rollback/confirm_good", mcufw_rollback_read_counter() == 0);
    mcufw_rollback_test_reset_state();
    mcufw_rollback_test_force_read_error(1);
    rb = mcufw_rollback_early_boot_check();
    check("rollback/read_error_fallback",
          mcufw_rollback_test_was_passive_activated() || rb == MCUFW_RB_ERROR);
    mcufw_rollback_test_force_read_error(0);
    mcufw_rollback_test_reset_state();
    mcufw_rollback_set_counter(4);
    mcufw_rollback_test_force_write_error(1);
    rb = mcufw_rollback_early_boot_check();
    check("rollback/write_error_at_threshold", rb == MCUFW_RB_ERROR);
    mcufw_rollback_test_force_write_error(0);
}

/* ---- Phase 4b.1: 4-channel RGBW PWM + GPIO44 -------------------------- */
static void run_pwm_rgbw(void)
{
    mcufw_pwm_stats_reset();
    mcufw_pwm_hal_stub_reset();
    mcufw_pwm_init();
    check("pwm_rgbw/init_gpio44_off", mcufw_pwm_hal_stub_gpio_get(44) == 0);
    mcufw_pwm_set(50, 3500);
    const mcufw_pwm_stats_t *st = mcufw_pwm_stats();
    check("pwm_rgbw/ct_mode", st->mode == MCUFW_PWM_MODE_CT);
    check("pwm_rgbw/ct_gpio44_on", mcufw_pwm_hal_stub_gpio_get(44) == 1);
    check("pwm_rgbw/ct_4ch_nonzero",
          st->ch_level[0] > 0 && st->ch_level[1] > 0 &&
          st->ch_level[2] > 0 && st->ch_level[3] > 0);
    mcufw_pwm_set(0, 3500);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/off_gpio44_low", mcufw_pwm_hal_stub_gpio_get(44) == 0);
    check("pwm_rgbw/off_all_zero",
          st->ch_level[0]==0 && st->ch_level[1]==0 &&
          st->ch_level[2]==0 && st->ch_level[3]==0);
    mcufw_pwm_hsv(120, 80, 75);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/hsv_mode", st->mode == MCUFW_PWM_MODE_HSV);
    check("pwm_rgbw/hsv_gpio44_on", mcufw_pwm_hal_stub_gpio_get(44) == 1);
    check("pwm_rgbw/hsv_green_dominant",
          st->ch_level[1] > st->ch_level[0] &&
          st->ch_level[1] > st->ch_level[2]);
    mcufw_pwm_hsv(120, 80, 0);
    check("pwm_rgbw/hsv_v0_gpio44_off", mcufw_pwm_hal_stub_gpio_get(44) == 0);
    mcufw_pwm_restore();
    st = mcufw_pwm_stats();
    check("pwm_rgbw/restore_gpio44_on", mcufw_pwm_hal_stub_gpio_get(44) == 1);
    check("pwm_rgbw/restore_powered", st->powered == 1);
    /* set_rgb: pure red (255,0,0) -> R=period, G=0, B=0, W=0 */
    mcufw_pwm_rgb(255, 0, 0);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/rgb_red_mode", st->mode == MCUFW_PWM_MODE_RGB);
    check("pwm_rgbw/rgb_red_gpio44_on", mcufw_pwm_hal_stub_gpio_get(44) == 1);
    check("pwm_rgbw/rgb_red_r_full", st->ch_level[0] == MCUFW_PWM_PERIOD);
    check("pwm_rgbw/rgb_red_gb_zero", st->ch_level[1] == 0 && st->ch_level[2] == 0);
    check("pwm_rgbw/rgb_red_w_zero", st->ch_level[3] == 0);

    /* set_rgb: white (255,255,255) -> all channels at period */
    mcufw_pwm_rgb(255, 255, 255);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/rgb_white_all_full",
          st->ch_level[0] == MCUFW_PWM_PERIOD && st->ch_level[1] == MCUFW_PWM_PERIOD &&
          st->ch_level[2] == MCUFW_PWM_PERIOD && st->ch_level[3] == MCUFW_PWM_PERIOD);

    /* set_rgb: black (0,0,0) -> off */
    mcufw_pwm_rgb(0, 0, 0);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/rgb_black_off", st->mode == MCUFW_PWM_MODE_OFF);
    check("pwm_rgbw/rgb_black_gpio44_off", mcufw_pwm_hal_stub_gpio_get(44) == 0);

    /* set_rgb: mixed color (128,64,32) -> W=min=32*period/255 */
    mcufw_pwm_rgb(128, 64, 32);
    st = mcufw_pwm_stats();
    check("pwm_rgbw/rgb_mixed_mode", st->mode == MCUFW_PWM_MODE_RGB);
    check("pwm_rgbw/rgb_mixed_w_extracted", st->ch_level[3] > 0 && st->ch_level[3] <= st->ch_level[2]);
    printf("  pwm_rgbw: reg_writes=%u skipped=%u service_applied=%u\n",
           st->reg_writes, st->skipped_identical, st->service_applied);

    /* Round-trip: set_rgb -> set_bright must preserve color (Review R3 fix) */
    {
        mcufw_state_t rst;
        const mcufw_pwm_stats_t *pst;
        mcufw_state_init(&rst);
        mcufw_state_set_power(&rst, 1);
        mcufw_state_set_rgb(&rst, 255, 0, 0);   /* pure red */
        mcufw_state_set_bright(&rst, 50);       /* brightness change triggers apply() */
        pst = mcufw_pwm_stats();
        check("pwm_rgbw/rgb_state_roundtrip_not_black",
              pst->ch_level[0] > 0 || pst->ch_level[1] > 0 || pst->ch_level[2] > 0 || pst->ch_level[3] > 0);
        check("pwm_rgbw/rgb_state_roundtrip_r_dominant",
              pst->ch_level[0] >= pst->ch_level[1] && pst->ch_level[0] >= pst->ch_level[2]);
        check("pwm_rgbw/rgb_state_roundtrip_mode_rgb",
              pst->mode == MCUFW_PWM_MODE_RGB);
    }
}


static void run_wlan_rtos(void);
static void run_devcfg_ota_portal(void);
int main(void)
{
    printf("=== mcufw phase 4 host test ===\n");

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

    printf("\n[PWM register writes]\n");
    run_pwm_register();

    printf("\n[LAN-RPC state mutation]\n");
    run_rpc_state();

    printf("\n[miIO state mutation]\n");
    run_miio_state();

    printf("\n[runtime owner / single writer]\n");
    run_runtime_owner();

    printf("\n[persistent config / power-on]\n");
    run_config_persist();

    printf("\n[Rollback early-boot]\n");
    run_rollback();
    printf("\n[PWM RGBW 4-ch + GPIO44]\n");
    run_pwm_rgbw();

    printf("\n[WLAN + RTOS state machine]\n");
    run_wlan_rtos();

    printf("\n[Device config + Self-OTA + HTTP portal]\n");
    run_devcfg_ota_portal();
    printf("\n=== TOTAL: %u PASS, %u FAIL ===\n", total_pass, total_fail);
    return total_fail == 0 ? 0 : 1;
}

/* ========================================================================== */
/* Phase 4b.2: WLAN + RTOS state machine tests (HAL seam)                     */
/* ========================================================================== */
#include "mcufw_wlan.h"

static int stub_init_ok, stub_connect_ok, stub_uap_ok;
static int stub_init_called, stub_connect_called, stub_uap_called;
static char stub_last_ssid[33], stub_last_psk[64];

static int stub_wlan_init(void) { stub_init_called++; return stub_init_ok ? 0 : -1; }
static int stub_wlan_connect(char *ssid, char *psk) {
    stub_connect_called++;
    if (ssid) { strncpy(stub_last_ssid, ssid, 32); stub_last_ssid[32]=0; }
    if (psk)  { strncpy(stub_last_psk, psk, 63); stub_last_psk[63]=0; }
    return stub_connect_ok ? 0 : -1;
}
static int stub_uap_start(const char *ssid, const char *psk) {
    stub_uap_called++;
    if (ssid) { strncpy(stub_last_ssid, ssid, 32); stub_last_ssid[32]=0; }
    (void)psk;
    return stub_uap_ok ? 0 : -1;
}
static void stub_wlan_deinit(void) {}

static const mcufw_wlan_hal_t test_hal = {
    stub_wlan_init, stub_wlan_connect, stub_uap_start, stub_wlan_deinit
};

static void reset_stubs(void) {
    stub_init_ok = 1; stub_connect_ok = 1; stub_uap_ok = 1;
    stub_init_called = 0; stub_connect_called = 0; stub_uap_called = 0;
    memset(stub_last_ssid, 0, sizeof(stub_last_ssid));
    memset(stub_last_psk, 0, sizeof(stub_last_psk));
    mcufw_wlan_set_hal(&test_hal);
}

static void run_wlan_rtos(void)
{
    mcufw_wlan_cfg_t cfg;
    mcufw_wlan_report_t rep;
    int rc;
    char ap_ssid[33];

    /* --- Setup AP SSID generator ----------------------------------------- */
    mcufw_wlan_setup_ap_ssid(ap_ssid, sizeof(ap_ssid), 0x05eb8034u);
    check("wlan/setup_ap_ssid_prefix", strncmp(ap_ssid, "yeelink-light-", 14) == 0);
    check("wlan/setup_ap_ssid_did", strstr(ap_ssid, "05eb8034") != NULL);

    /* --- Unprovisioned -> AP fallback ------------------------------------ */
    reset_stubs();
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 0;
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/unprov_rc_ok", rc == MCUFW_WLAN_OK);
    check("wlan/unprov_state_ap", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_AP_FALLBACK);
    check("wlan/unprov_uap_called", stub_uap_called == 1);
    check("wlan/unprov_connect_not_called", stub_connect_called == 0);
    check("wlan/unprov_init_called", stub_init_called == 1);

    /* --- Provisioned + connect OK -> STA connected ----------------------- */
    reset_stubs();
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 1;
    strcpy(cfg.ssid, "TestNet");
    strcpy(cfg.psk, "12345678");
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/sta_ok_rc", rc == MCUFW_WLAN_OK);
    check("wlan/sta_ok_state", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_STA_CONNECTED);
    check("wlan/sta_ok_connect_called", stub_connect_called == 1);
    check("wlan/sta_ok_ssid", strcmp(stub_last_ssid, "TestNet") == 0);
    check("wlan/sta_ok_uap_not_called", stub_uap_called == 0);

    /* --- Provisioned + connect FAIL -> AP fallback ----------------------- */
    reset_stubs();
    stub_connect_ok = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 1;
    strcpy(cfg.ssid, "FailNet");
    strcpy(cfg.psk, "abcdefgh");
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/sta_fail_rc_ok", rc == MCUFW_WLAN_OK);
    check("wlan/sta_fail_state_ap", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_AP_FALLBACK);
    check("wlan/sta_fail_connect_called", stub_connect_called == 1);
    check("wlan/sta_fail_uap_called", stub_uap_called == 1);

    /* --- wlan_init fail -> ERROR state ----------------------------------- */
    reset_stubs();
    stub_init_ok = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 0;
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/init_fail_state_error", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_ERROR);

    /* --- Invalid SSID when provisioned=1 -> E_SSID ----------------------- */
    reset_stubs();
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 1;
    cfg.ssid[0] = 0; /* empty SSID */
    strcpy(cfg.psk, "12345678");
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/bad_ssid_rc", rc == MCUFW_WLAN_E_SSID);
    check("wlan/bad_ssid_state_error", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_ERROR);

    /* --- Invalid PSK when provisioned=1 -> E_PSK ------------------------- */
    reset_stubs();
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MCUFW_WLAN_NVRAM_MAGIC;
    cfg.provisioned = 1;
    strcpy(cfg.ssid, "ValidNet");
    strcpy(cfg.psk, "short"); /* < 8 chars */
    rc = mcufw_wlan_bringup(&cfg, &rep);
    check("wlan/bad_psk_rc", rc == MCUFW_WLAN_E_PSK);
    check("wlan/bad_psk_state_error", mcufw_wlan_get_state() == MCUFW_WLAN_STATE_ERROR);

    /* --- Deferred rollback confirmation (only after threshold & wlan) ---- */
    mcufw_rollback_test_reset_state();
    mcufw_rollback_set_counter(3u);
    uint32_t ticks = 0u;
    int confirmed = 0;
    check("wlan/confirm_not_ready_stays_3",
          mcufw_rollback_deferred_tick(&ticks, &confirmed, 0, 5u) == 0 &&
          confirmed == 0 && mcufw_rollback_read_counter() == 3u);
    for (unsigned i = 0; i < 4u; i++) {
        (void)mcufw_rollback_deferred_tick(&ticks, &confirmed, 1, 5u);
    }
    check("wlan/confirm_before_threshold_stays_3",
          confirmed == 0 && mcufw_rollback_read_counter() == 3u);
    check("wlan/confirm_at_threshold_clears",
          mcufw_rollback_deferred_tick(&ticks, &confirmed, 1, 5u) == 1 &&
          confirmed == 1 && mcufw_rollback_read_counter() == 0u);
    mcufw_rollback_set_counter(2u);
    check("wlan/confirm_once_only",
          mcufw_rollback_deferred_tick(&ticks, &confirmed, 1, 5u) == 0 &&
          mcufw_rollback_read_counter() == 2u);

    /* Restore default HAL for any subsequent tests */
    mcufw_wlan_set_hal(NULL);
}


/* ========================================================================== */
/* Phase 4b.3: Device config + Self-OTA + HTTP portal tests                   */
/* ========================================================================== */
extern void mcufw_ota_hal_stub_reset(const uint8_t *payload, size_t payload_len,
                                     int http_status, uint32_t declared_len,
                                     uint8_t chunked, uint32_t slot_max);
extern void mcufw_ota_hal_stub_set_open_err(int err);
extern uint32_t mcufw_ota_hal_stub_begin_calls(void);
extern uint32_t mcufw_ota_hal_stub_abort_calls(void);
extern uint32_t mcufw_ota_hal_stub_complete_calls(void);
extern uint32_t mcufw_ota_hal_stub_reboot_calls(void);
extern int      mcufw_ota_hal_stub_active_slot(void);
extern size_t   mcufw_ota_hal_stub_written(void);

static void md5_to_hex(const uint8_t d[16], char out[33])
{
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16u; i++) {
        out[i * 2u] = hx[(d[i] >> 4) & 0xfu];
        out[i * 2u + 1u] = hx[d[i] & 0xfu];
    }
    out[32] = '\0';
}

static void run_devcfg_ota_portal(void)
{
    char json[640], rsp[1536];
    size_t rlen = 0;
    mcufw_state_t st;

    /* --- 1. Device config defaults, apply, masking, roundtrip, power-on -- */
    mcufw_config_hal_stub_reset();
    check("devcfg/boot_defaults", mcufw_devcfg_boot() != 0 &&
          strcmp(mcufw_devcfg()->mqtt_host, "192.168.1.106") == 0 &&
          mcufw_devcfg()->mqtt_port == 1883u &&
          mcufw_devcfg()->pwm_freq_hz == 768u);

    const char *good_cfg =
        "{\"wifi_ssid\":\"MyHomeWiFi\",\"wifi_psk\":\"SuperSecret88\","
        "\"mqtt_host\":\"192.168.1.106\",\"mqtt_port\":1883,"
        "\"mqtt_user\":\"yeelight\",\"mqtt_pass\":\"MqttPass99\","
        "\"power_on_mode\":\"off\",\"pwm_freq_hz\":4000,"
        "\"default_transition_ms\":750,\"gamma_enable\":1}";
    check("devcfg/apply_valid_json",
          mcufw_devcfg_apply_json(good_cfg, strlen(good_cfg)) == 0);
    check("devcfg/wlan_synced",
          mcufw_wlan_cfg()->provisioned == 1u &&
          strcmp(mcufw_wlan_cfg()->ssid, "MyHomeWiFi") == 0);

    check("devcfg/to_json_masks_secrets",
          mcufw_devcfg_to_json(mcufw_devcfg(), json, sizeof(json)) > 0 &&
          strstr(json, "SuperSecret88") == NULL &&
          strstr(json, "MqttPass99") == NULL &&
          strstr(json, "\"wifi_psk\":\"***\"") != NULL &&
          strstr(json, "\"mqtt_pass\":\"***\"") != NULL);

    /* Reboot reload preserves config + masked "***" update keeps secrets */
    mcufw_devcfg_defaults(mcufw_devcfg());
    check("devcfg/persist_roundtrip",
          mcufw_devcfg_boot() == 0 &&
          strcmp(mcufw_devcfg()->wifi_ssid, "MyHomeWiFi") == 0 &&
          strcmp(mcufw_devcfg()->wifi_psk, "SuperSecret88") == 0 &&
          mcufw_devcfg()->pwm_freq_hz == 4000u &&
          mcufw_devcfg()->power_on_mode == MCUFW_POWERON_OFF);

    const char *masked_update = "{\"wifi_psk\":\"***\",\"pwm_freq_hz\":8000}";
    check("devcfg/masked_keeps_psk",
          mcufw_devcfg_apply_json(masked_update, strlen(masked_update)) == 0 &&
          strcmp(mcufw_devcfg()->wifi_psk, "SuperSecret88") == 0 &&
          mcufw_devcfg()->pwm_freq_hz == 8000u);

    /* Invalid configs are atomically rejected */
    const char *bad_psk = "{\"wifi_ssid\":\"BadNet\",\"wifi_psk\":\"123\"}";
    check("devcfg/reject_short_psk",
          mcufw_devcfg_apply_json(bad_psk, strlen(bad_psk)) != 0 &&
          strcmp(mcufw_devcfg()->wifi_ssid, "MyHomeWiFi") == 0);
    const char *bad_freq = "{\"pwm_freq_hz\":99999}";
    check("devcfg/reject_bad_pwm_freq",
          mcufw_devcfg_apply_json(bad_freq, strlen(bad_freq)) != 0 &&
          mcufw_devcfg()->pwm_freq_hz == 8000u);

    /* Power-on mode off / default */
    mcufw_state_init(&st);
    st.power = 1u; st.bright = 45u;
    mcufw_devcfg_apply_power_on(&st);
    check("devcfg/poweron_off_forces_off", st.power == 0u);
    check("devcfg/poweron_default",
          mcufw_devcfg_set_power_on_mode(MCUFW_POWERON_DEFAULT) == 0 &&
          mcufw_devcfg_apply_power_on(&st) == 0 &&
          st.power == 1u && st.bright == 100u && st.ct == 4000u);

    /* --- 2. Build a synthetic MRVL test image for OTA tests --------------- */
    static uint8_t fake_fw[1024];
    memset(fake_fw, 0xA5, sizeof(fake_fw));
    fake_fw[0] = 'M'; fake_fw[1] = 'R'; fake_fw[2] = 'V'; fake_fw[3] = 'L';
    fake_fw[4] = 0x7bu; fake_fw[5] = 0xf1u; fake_fw[6] = 0x9cu; fake_fw[7] = 0x2eu;
    fake_fw[12] = 3u; fake_fw[13] = 0u; fake_fw[14] = 0u; fake_fw[15] = 0u;
    uint8_t digest[16];
    char md5_ok[33], md5_bad[33];
    mcufw_md5(fake_fw, sizeof(fake_fw), digest);
    md5_to_hex(digest, md5_ok);
    strcpy(md5_bad, md5_ok);
    md5_bad[0] = (md5_bad[0] == 'a') ? 'b' : 'a';

    mcufw_ota_set_hal(NULL);

    /* Happy path */
    mcufw_ota_reset();
    mcufw_rollback_test_reset_state();
    mcufw_rollback_set_counter(3u);
    mcufw_ota_hal_stub_reset(fake_fw, sizeof(fake_fw), 200, sizeof(fake_fw), 0u, 0x60000u);
    check("ota/happy_path_ok",
          mcufw_ota_execute("http://192.168.1.10/fw.bin", md5_ok) == 0);
    check("ota/happy_slot_switched_rebooted",
          mcufw_ota_state()->status == MCUFW_OTA_DONE &&
          mcufw_ota_hal_stub_written() == sizeof(fake_fw) &&
          mcufw_ota_hal_stub_complete_calls() == 1u &&
          mcufw_ota_hal_stub_abort_calls() == 0u &&
          mcufw_ota_hal_stub_reboot_calls() == 1u &&
          mcufw_ota_hal_stub_active_slot() == 1 &&
          mcufw_rollback_read_counter() == 0u);

    /* HTTP 404 dry-run safety: never even calls slot_begin! */
    mcufw_ota_reset();
    mcufw_ota_hal_stub_reset(fake_fw, sizeof(fake_fw), 404, sizeof(fake_fw), 0u, 0x60000u);
    check("ota/404_never_touches_flash",
          mcufw_ota_execute("http://192.168.1.10/missing.bin", md5_ok) < 0 &&
          mcufw_ota_state()->status == MCUFW_OTA_ERR &&
          mcufw_ota_hal_stub_begin_calls() == 0u &&
          mcufw_ota_hal_stub_complete_calls() == 0u &&
          mcufw_ota_hal_stub_active_slot() == 0);

    /* Bad MRVL magic: aborted BEFORE slot_begin! */
    uint8_t bad_magic_fw[128];
    memcpy(bad_magic_fw, fake_fw, sizeof(bad_magic_fw));
    bad_magic_fw[0] = 'X';
    mcufw_ota_reset();
    mcufw_ota_hal_stub_reset(bad_magic_fw, sizeof(bad_magic_fw), 200, sizeof(bad_magic_fw), 0u, 0x60000u);
    check("ota/bad_magic_no_flash",
          mcufw_ota_execute("http://192.168.1.10/bad.bin", "") < 0 &&
          mcufw_ota_hal_stub_begin_calls() == 0u);

    /* Oversize guard (> slot_max): aborted before flash writes! */
    mcufw_ota_reset();
    mcufw_ota_hal_stub_reset(fake_fw, sizeof(fake_fw), 200, 0x70000u, 0u, 0x60000u);
    check("ota/oversize_rejected",
          mcufw_ota_execute("http://192.168.1.10/big.bin", "") < 0 &&
          mcufw_ota_hal_stub_written() == 0u &&
          mcufw_ota_hal_stub_complete_calls() == 0u);

    /* Bad MD5: slot_abort called, zero slot switch or reboot */
    mcufw_ota_reset();
    mcufw_ota_hal_stub_reset(fake_fw, sizeof(fake_fw), 200, sizeof(fake_fw), 0u, 0x60000u);
    check("ota/bad_md5_aborts_no_switch",
          mcufw_ota_execute("http://192.168.1.10/fw.bin", md5_bad) < 0 &&
          mcufw_ota_hal_stub_abort_calls() == 1u &&
          mcufw_ota_hal_stub_complete_calls() == 0u &&
          mcufw_ota_hal_stub_reboot_calls() == 0u &&
          mcufw_ota_hal_stub_active_slot() == 0);

    /* HTTP Portal endpoints (GET /, GET /api/config, POST /api/ota, POST /api/rollback) */
    mcufw_ota_reset();
    const char *req_get = "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
    check("http/get_root_html",
          mcufw_http_handle_request(req_get, strlen(req_get), rsp, sizeof(rsp), &rlen) == 0 &&
          strstr(rsp, "200 OK") != NULL && strstr(rsp, "<html") != NULL);

    char req_ota[512];
    snprintf(req_ota, sizeof(req_ota),
             "POST /api/ota HTTP/1.1\r\nContent-Length: 90\r\n\r\n"
             "{\"app_url\":\"http://192.168.1.10/new.bin\",\"file_md5\":\"%s\"}",
             md5_ok);
    mcufw_ota_hal_stub_reset(fake_fw, sizeof(fake_fw), 200, sizeof(fake_fw), 0u, 0x60000u);
    check("http/post_ota_queued_and_serviced",
          mcufw_http_handle_request(req_ota, strlen(req_ota), rsp, sizeof(rsp), &rlen) == 0 &&
          strstr(rsp, "202 Accepted") != NULL &&
          mcufw_ota_state()->status == MCUFW_OTA_QUEUED &&
          mcufw_ota_service() == 1 &&
          mcufw_ota_state()->status == MCUFW_OTA_DONE);

    mcufw_ota_reset();
    const char *req_rb = "POST /api/rollback HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}";
    check("http/post_rollback_scheduled",
          mcufw_http_handle_request(req_rb, strlen(req_rb), rsp, sizeof(rsp), &rlen) == 0 &&
          strstr(rsp, "rollback_scheduled") != NULL &&
          mcufw_ota_state()->rollback_pending == 1);
    mcufw_ota_reset();
}
