/*
 * mcufw_lan_rpc.c -- LAN JSON-RPC dispatcher for TCP 55443.
 *
 * Implements the Yeelight LAN control protocol subset. Methods that must
 * remain cloud/miIO-only (set_ps, get_ps, cfg_lan_ctrl, ota) return
 * code 2 ("method not supported") per the color1_0061 whitelist.
 *
 * Phase 3.5 scope: dispatch skeleton + whitelist enforcement. Real lamp
 * state mutation is wired in phase 3.6/4; here we acknowledge valid
 * methods with a stub OK response so the protocol contract is testable.
 */
#include "mcufw_lan_rpc.h"

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
static int rpc_respond(uint32_t id, int have_id, int code, const char *result,
                       char *out, size_t cap)
{
    static const char hdr[] = "{\"id\":";
    static const char err_pre[] = ",\"error\":{\"code\":";
    static const char err_post[] = ",\"message\":\"method not supported\"}}";
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
        for (unsigned i = 0; i < sizeof(err_post) - 1 && pos < cap; i++)
            out[pos++] = err_post[i];
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
    return (int)pos;
}

int mcufw_rpc_dispatch(const mcufw_rpc_request_t *req,
                       char *out, size_t out_cap)
{
    if (!req || !out || out_cap == 0)
        return MCUFW_RPC_E_INTERNAL;

    if (!mcufw_rpc_method_allowed(req->method))
        return rpc_respond(req->id, req->have_id, MCUFW_RPC_E_METHOD, NULL,
                           out, out_cap);

    return rpc_respond(req->id, req->have_id, MCUFW_RPC_OK, "\"ok\"",
                       out, out_cap);
}
