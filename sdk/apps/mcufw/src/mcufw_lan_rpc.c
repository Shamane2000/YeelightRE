/*
 * mcufw_lan_rpc.c -- LAN JSON-RPC dispatcher for TCP 55443.
 *
 * Implements the Yeelight LAN control protocol subset. Methods that must
 * remain cloud/miIO-only (set_ps, get_ps, cfg_lan_ctrl, ota) return
 * code 2 ("method not supported") per the color1_0061 whitelist.
 *
 * Phase 3.6: the allowed methods no longer answer a stub "ok" -- they mutate
 * the internal lamp state (mcufw_state) and push the result into the PWM
 * backend.  Every setter response is wire compatible with real clients:
 * result[0] stays the literal "ok" (python-yeelight/HA only inspect that
 * element), and the clamped state value follows as result[1] so a controller
 * can verify what the firmware actually accepted (e.g. set_bright 150 ->
 * ["ok",100]).  get_prop answers from the same state, so a read-back after a
 * write proves the mutation.
 */
#include "mcufw_cf.h"
#include "mcufw_lan_rpc.h"
#include "mcufw_pwm.h"
#include "mcufw_state.h"

/* ---- minimal JSON helpers (no libc, no dynamic alloc) ------------------- */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

/* Find key "key" in a flat JSON object buffer, return pointer to value start. */
static const char *json_find(const char *buf, size_t len, const char *key)
{
    unsigned klen = str_len(key);
    for (size_t i = 0; i + klen + 2 < len; i++) {
        if (buf[i] == '"' && i + 1 + klen < len && buf[i+1+klen] == '"') {
            int match = 1;
            for (unsigned j = 0; j < klen; j++) {
                if (buf[i+1+j] != key[j]) { match = 0; break; }
            }
            if (match) {
                size_t p = i + klen + 2;
                while (p < len && (buf[p] == ' ' || buf[p] == ':')) p++;
                if (p < len) return &buf[p];
            }
        }
    }
    return NULL;
}

static unsigned json_str_val(const char *src, size_t avail, char *dst, size_t cap)
{
    if (!src || avail == 0 || *src != '"') return 0;
    src++; avail--;
    unsigned n = 0;
    while (n < avail && n + 1 < cap && src[n] != '"') {
        dst[n] = src[n]; n++;
    }
    dst[n] = '\0';
    return n;
}

static uint32_t json_int_val(const char *src, size_t avail)
{
    uint32_t v = 0;
    if (!src) return 0;
    while (avail && (*src == ' ' || *src == '"')) { src++; avail--; }
    while (avail && *src >= '0' && *src <= '9') {
        v = v * 10 + (uint32_t)(*src - '0');
        src++; avail--;
    }
    return v;
}

/* ---- params array access ------------------------------------------------ *
 * `params` is the raw JSON substring after "params" (e.g. `["on","smooth",
 * 500]`).  p_elem() returns the idx-th element:
 *   1 = JSON string copied into out/out_cap
 *   2 = number stored in *num
 *   0 = element missing (or an object/array element, which no supported
 *       method needs)
 * Nested objects/arrays are skipped by bracket counting, so a name such as
 * "1,2,3" inside a string is not mistaken for a separator.
 */
static int p_elem(const char *s, unsigned idx, char *out, size_t out_cap,
                  long *num)
{
    unsigned cur = 0;

    if (!s)
        return 0;

    while (*s == ' ' || *s == '[')
        s++;

    while (*s) {
        if (*s == '"') {
            const char *q = s + 1;
            size_t n = 0;
            while (*q && *q != '"') {
                if (out && n + 1 < out_cap)
                    out[n] = *q;
                n++;
                q++;
            }
            if (out && out_cap)
                out[n < out_cap ? n : out_cap - 1] = '\0';
            if (cur == idx)
                return 1;
            if (*q)
                q++;
            s = q;
        } else if (*s == '{' || *s == '[') {
            int depth = 0;
            const char *q = s;
            while (*q) {
                if (*q == '{' || *q == '[')
                    depth++;
                else if (*q == '}' || *q == ']') {
                    depth--;
                    if (depth == 0) { q++; break; }
                }
                q++;
            }
            if (cur == idx)
                return 0;
            s = q;
        } else if (*s == '-' || (*s >= '0' && *s <= '9')) {
            long v = 0;
            int neg = 0;
            while (*s == '-') { neg = !neg; s++; }
            while (*s >= '0' && *s <= '9') {
                v = v * 10 + (long)(*s - '0');
                s++;
            }
            if (num)
                *num = neg ? -v : v;
            if (cur == idx)
                return 2;
        } else {
            /* ']' , ',' or stray byte */
            s++;
        }

        while (*s == ' ' || *s == ',')
            s++;
        cur++;
    }
    return 0;
}

/* ---- result builder (flat JSON array content) --------------------------- */
struct rbuf {
    char  *p;
    size_t cap;
    size_t len;
    int    ovf;
};

static void rb_put(struct rbuf *b, const char *s)
{
    while (*s) {
        if (b->len + 1u >= b->cap) { b->ovf = 1; return; }
        b->p[b->len++] = *s++;
    }
    b->p[b->len] = '\0';
}

static void rb_sep(struct rbuf *b)
{
    if (b->len)
        rb_put(b, ",");
}

static void rb_num(struct rbuf *b, long v)
{
    char tmp[12];
    int n = 0;
    unsigned long u;

    if (v < 0) {
        rb_put(b, "-");
        u = (unsigned long)(-(v + 1)) + 1ul;
    } else {
        u = (unsigned long)v;
    }
    do {
        tmp[n++] = (char)('0' + (int)(u % 10ul));
        u /= 10ul;
    } while (u && n < (int)sizeof(tmp));
    {
        char num[16];
        int k = 0;
        while (n > 0)
            num[k++] = tmp[--n];
        num[k] = '\0';
        rb_put(b, num);
    }
}

static void rb_str(struct rbuf *b, const char *s)
{
    rb_put(b, "\"");
    rb_put(b, s);
    rb_put(b, "\"");
}

static void rb_ok(struct rbuf *b)
{
    rb_str(b, "ok");
}

/* ---- whitelist ---------------------------------------------------------- */
static const char *blocked_methods[] = {
    "set_ps", "get_ps", "cfg_lan_ctrl", "ota"
};

int mcufw_rpc_method_allowed(const char *method)
{
    for (unsigned i = 0; i < sizeof(blocked_methods)/sizeof(blocked_methods[0]); i++) {
        if (str_eq(method, blocked_methods[i]))
            return 0;
    }
    return 1;
}

/* ---- parser ------------------------------------------------------------- */
int mcufw_rpc_parse(const char *buf, size_t len, mcufw_rpc_request_t *req)
{
    const char *mp, *ip, *pp;

    if (!buf || !req || len == 0)
        return MCUFW_RPC_E_PARSE;

    mp = json_find(buf, len, "method");
    if (!mp)
        return MCUFW_RPC_E_PARSE;
    if (json_str_val(mp, len - (size_t)(mp - buf), req->method, sizeof(req->method)) == 0)
        return MCUFW_RPC_E_PARSE;

    ip = json_find(buf, len, "id");
    if (ip) {
        req->id = json_int_val(ip, len - (size_t)(ip - buf));
        req->have_id = 1;
    } else {
        req->id = 0;
        req->have_id = 0;
    }

    pp = json_find(buf, len, "params");
    if (pp) {
        size_t plen = len - (size_t)(pp - buf);
        if (plen > sizeof(req->params) - 1)
            plen = sizeof(req->params) - 1;
        for (size_t i = 0; i < plen; i++)
            req->params[i] = pp[i];
        req->params[plen] = '\0';
    } else {
        req->params[0] = '\0';
    }

    return MCUFW_RPC_OK;
}

/* ---- dispatcher --------------------------------------------------------- */
static int rpc_respond(uint32_t id, int have_id, int code, const char *message,
                       const char *result, char *out, size_t cap)
{
    static const char hdr[] = "{\"id\":";
    static const char err_pre[] = ",\"error\":{\"code\":";
    static const char err_msg[] = ",\"message\":\"";
    static const char ok_pre[] = ",\"result\":[";
    static const char ok_post[] = "]}";
    unsigned pos = 0;

    if (!have_id)
        return 0;

    for (unsigned i = 0; i < sizeof(hdr) - 1 && pos < cap; i++)
        out[pos++] = hdr[i];
    {
        char ibuf[11]; int ilen = 0; uint32_t v = id;
        if (v == 0) { ibuf[ilen++] = '0'; }
        else { while (v) { ibuf[ilen++] = (char)('0' + v % 10); v /= 10; } }
        while (ilen > 0 && pos < cap) out[pos++] = ibuf[--ilen];
    }

    if (code != 0) {
        for (unsigned i = 0; i < sizeof(err_pre) - 1 && pos < cap; i++)
            out[pos++] = err_pre[i];
        {
            int c = -code;
            if (c == 0) c = 1;
            char cb[4]; int cl = 0;
            while (c) { cb[cl++] = (char)('0' + c % 10); c /= 10; }
            while (cl > 0 && pos < cap) out[pos++] = cb[--cl];
        }
        for (unsigned i = 0; i < sizeof(err_msg) - 1 && pos < cap; i++)
            out[pos++] = err_msg[i];
        if (message) {
            unsigned mlen = str_len(message);
            for (unsigned i = 0; i < mlen && pos < cap; i++)
                out[pos++] = message[i];
        }
        for (unsigned i = 0; i < 3 && pos < cap; i++)
            out[pos++] = "\"}}"[i];
    } else {
        for (unsigned i = 0; i < sizeof(ok_pre) - 1 && pos < cap; i++)
            out[pos++] = ok_pre[i];
        if (result) {
            unsigned rlen = str_len(result);
            for (unsigned i = 0; i < rlen && pos < cap; i++)
                out[pos++] = result[i];
        }
        for (unsigned i = 0; i < sizeof(ok_post) - 1 && pos < cap; i++)
            out[pos++] = ok_post[i];
    }
    out[pos < cap ? pos : cap - 1] = '\0';
    return (int)pos;
}

/* get_prop: answer the requested properties from the live state. */
static void rpc_build_props(const mcufw_state_t *st, const char *params,
                            struct rbuf *rb)
{
    char prop[24];

    for (unsigned idx = 0; ; idx++) {
        int kind = p_elem(params, idx, prop, sizeof(prop), NULL);

        if (kind != 1)
            break;
        rb_sep(rb);
        if (str_eq(prop, "power")) {
            rb_str(rb, st->power ? "on" : "off");
        } else if (str_eq(prop, "bright")) {
            rb_num(rb, (long)st->bright);
        } else if (str_eq(prop, "ct")) {
            rb_num(rb, (long)st->ct);
        } else if (str_eq(prop, "hue")) {
            rb_num(rb, (long)st->hue);
        } else if (str_eq(prop, "sat")) {
            rb_num(rb, (long)st->sat);
        } else if (str_eq(prop, "name")) {
            rb_str(rb, st->name);
        } else if (str_eq(prop, "color_mode") || str_eq(prop, "mode")) {
            rb_num(rb, (long)st->color_mode);
        } else if (str_eq(prop, "cf")) {
            rb_num(rb, (long)st->cf_active);
        } else if (str_eq(prop, "hsv")) {
            char hsv[24];
            struct rbuf sub;
            sub.p = hsv; sub.cap = sizeof(hsv); sub.len = 0; sub.ovf = 0;
            rb_num(&sub, (long)st->hue);
            rb_put(&sub, ",");
            rb_num(&sub, (long)st->sat);
            rb_put(&sub, ",");
            rb_num(&sub, (long)st->bright);
            rb_str(rb, hsv);
        } else {
            rb_str(rb, "");
        }
    }
}

int mcufw_rpc_dispatch(const mcufw_rpc_request_t *req,
                       char *out, size_t out_cap)
{
    mcufw_state_t *st;
    char result[192];
    char s1[MCUFW_NAME_MAX + 1];
    struct rbuf rb;
    long n1 = 0, n2 = 0, n3 = 0;

    if (!req || !out || out_cap == 0)
        return MCUFW_RPC_E_INTERNAL;

    if (!mcufw_rpc_method_allowed(req->method))
        return rpc_respond(req->id, req->have_id, MCUFW_RPC_E_METHOD,
                           "method not supported", NULL, out, out_cap);

    st = mcufw_state();
    rb.p = result;
    rb.cap = sizeof(result);
    rb.len = 0;
    rb.ovf = 0;

    if (str_eq(req->method, "set_power")) {
        int on;

        if (p_elem(req->params, 0, s1, sizeof(s1), NULL) != 1)
            goto bad_param;
        if (str_eq(s1, "on"))
            on = 1;
        else if (str_eq(s1, "off"))
            on = 0;
        else
            goto bad_param;
        mcufw_state_set_power(st, on);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, (long)st->power);
    } else if (str_eq(req->method, "toggle")) {
        mcufw_state_toggle(st);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, (long)st->power);
    } else if (str_eq(req->method, "set_bright")) {
        if (p_elem(req->params, 0, NULL, 0, &n1) != 2)
            goto bad_param;
        mcufw_state_set_bright(st, n1);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, (long)st->bright);
    } else if (str_eq(req->method, "set_ct_abx")) {
        if (p_elem(req->params, 0, NULL, 0, &n1) != 2)
            goto bad_param;
        mcufw_state_set_ct(st, n1);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, (long)st->ct);
    } else if (str_eq(req->method, "set_hsv")) {
        if (p_elem(req->params, 0, NULL, 0, &n1) != 2)
            goto bad_param;
        if (p_elem(req->params, 1, NULL, 0, &n2) != 2)
            goto bad_param;
        if (p_elem(req->params, 2, NULL, 0, &n3) != 2)
            n3 = (long)st->bright; /* third arg may be "smooth" */
        mcufw_state_set_hsv(st, n1, n2, n3);
        rb_ok(&rb);
        rb_sep(&rb); rb_num(&rb, (long)st->hue);
        rb_sep(&rb); rb_num(&rb, (long)st->sat);
        rb_sep(&rb); rb_num(&rb, (long)st->bright);
    } else if (str_eq(req->method, "set_rgb")) {
        /* Yeelight RGB integer encoding: (r<<16)|(g<<8)|b */
        if (p_elem(req->params, 0, NULL, 0, &n1) != 2)
            goto bad_param;
        uint8_t rr = (uint8_t)((n1 >> 16) & 0xFF);
        uint8_t gg = (uint8_t)((n1 >> 8) & 0xFF);
        uint8_t bb = (uint8_t)(n1 & 0xFF);
        mcufw_state_set_rgb(st, (long)rr, (long)gg, (long)bb);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, n1);
    } else if (str_eq(req->method, "set_name")) {
        if (p_elem(req->params, 0, s1, sizeof(s1), NULL) != 1)
            goto bad_param;
        mcufw_state_set_name(st, s1);
        rb_ok(&rb); rb_sep(&rb); rb_str(&rb, st->name);
    } else if (str_eq(req->method, "start_cf")) {
        mcufw_cf_params_t cf;

        if (mcufw_cf_parse(req->params, str_len(req->params), &cf) != 0)
            goto bad_param;
        (void)p_elem(req->params, 0, NULL, 0, &n1);
        (void)p_elem(req->params, 1, NULL, 0, &n2);
        cf.count = (uint32_t)(n1 < 0 ? 0 : n1);
        cf.action = (uint32_t)(n2 < 0 ? 0 : n2);
        if (mcufw_cf_start(&cf) != 0)
            goto bad_param;
        mcufw_state_set_cf(st, cf.action != MCUFW_CF_ACTION_OFF);
        rb_ok(&rb); rb_sep(&rb); rb_num(&rb, (long)st->cf_active);
    } else if (str_eq(req->method, "set_default")) {
        mcufw_state_set_default(st, 1);
        rb_ok(&rb);
    } else if (str_eq(req->method, "get_prop")) {
        rpc_build_props(st, req->params, &rb);
    } else {
        /* Method passes the whitelist gate but has no handler yet
         * (cron_*, ...) -- answered as "ok" like the 3.5 stub. */
        rb_ok(&rb);
    }

    if (rb.ovf)
        return MCUFW_RPC_E_INTERNAL;

    return rpc_respond(req->id, req->have_id, MCUFW_RPC_OK, NULL, result,
                       out, out_cap);

bad_param:
    return rpc_respond(req->id, req->have_id, MCUFW_RPC_E_PARAM,
                       "invalid params", NULL, out, out_cap);
}
