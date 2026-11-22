/*
 * mcufw_http_portal.c -- Setup-AP / LAN HTTP Config + OTA endpoint handler.
 */
#include "mcufw_http_portal.h"
#include "mcufw_devcfg.h"
#include "mcufw_ota.h"
#include "mcufw_state.h"
#include "mcufw_wlan.h"

static void s_put(char *o, size_t cap, size_t *p, const char *s)
{
	while (s && *s && *p + 1u < cap) o[(*p)++] = *s++;
	o[*p] = '\0';
}

static void s_dec(char *o, size_t cap, size_t *p, uint32_t v)
{
	char b[12];
	int i = 0;
	if (v == 0u) b[i++] = '0';
	else while (v && i < 11) { b[i++] = (char)('0' + (v % 10u)); v /= 10u; }
	while (i-- > 0 && *p + 1u < cap) o[(*p)++] = b[i];
	o[*p] = '\0';
}

static int starts_with(const char *s, size_t len, const char *p)
{
	size_t i = 0u;
	while (p[i]) {
		if (i >= len || s[i] != p[i]) return 0;
		i++;
	}
	return 1;
}

static const char *find_body(const char *req, size_t len, size_t *blen)
{
	for (size_t i = 0u; i + 3u < len; i++) {
		if (req[i] == '\r' && req[i + 1u] == '\n' &&
		    req[i + 2u] == '\r' && req[i + 3u] == '\n') {
			*blen = len - (i + 4u);
			return req + i + 4u;
		}
	}
	*blen = 0u;
	return NULL;
}

/* Minimal JSON string extractor shared for POST /api/ota */
static int json_str_val(const char *j, size_t len, const char *key,
                        char *out, size_t cap)
{
	size_t klen = 0u;
	while (key[klen]) klen++;

	for (size_t i = 0u; i + klen + 4u < len; i++) {
		size_t k;
		if (j[i] != '"') continue;
		for (k = 0u; k < klen && j[i + 1u + k] == key[k]; k++) ;
		if (k != klen || j[i + 1u + k] != '"') continue;
		size_t p = i + 2u + klen;
		while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == ':')) p++;
		if (p >= len || j[p] != '"') continue;
		p++;
		size_t o = 0u;
		while (p < len && j[p] != '"') {
			char c = j[p++];
			if (c == '\\' && p < len) c = j[p++];
			if (o + 1u < cap) out[o++] = c;
		}
		if (p < len && j[p] == '"') {
			out[o] = '\0';
			return 0;
		}
		return -1;
	}
	return -1;
}

static int build_http(char *rsp, size_t cap, size_t *out_len,
                      const char *status, const char *ctype, const char *body)
{
	size_t p = 0u, blen = 0u;
	while (body && body[blen]) blen++;

	s_put(rsp, cap, &p, "HTTP/1.1 ");
	s_put(rsp, cap, &p, status);
	s_put(rsp, cap, &p, "\r\nContent-Type: ");
	s_put(rsp, cap, &p, ctype);
	s_put(rsp, cap, &p, "\r\nContent-Length: ");
	s_dec(rsp, cap, &p, (uint32_t)blen);
	s_put(rsp, cap, &p, "\r\nConnection: close\r\n\r\n");
	if (p + blen >= cap) {
		*out_len = 0u;
		return -1;
	}
	s_put(rsp, cap, &p, body);
	*out_len = p;
	return 0;
}

int mcufw_http_handle_request(const char *req, size_t req_len,
                              char *rsp, size_t rsp_cap, size_t *rsp_len)
{
	char body_buf[640];
	size_t blen = 0u;
	const char *body;

	if (!req || !rsp || !rsp_len || req_len < 5u || rsp_cap < 256u)
		return -1;
	body = find_body(req, req_len, &blen);

	if (starts_with(req, req_len, "GET / ") || starts_with(req, req_len, "GET /index.html ")) {
		static const char html[] =
			"<!doctype html><html><meta charset=utf-8><title>Yeelight mcufw</title>"
			"<h3>Yeelight Custom FW</h3>"
			"<p>API: <code>GET/POST /api/config</code>, <code>POST /api/ota</code>, "
			"<code>POST /api/rollback</code>, <code>GET /api/status</code></p>"
			"<input id=s placeholder=SSID><input id=p type=password placeholder=PSK>"
			"<input id=m value=192.168.1.106 placeholder=MQTT>"
			"<button onclick=\"fetch('/api/config',{method:'POST',body:JSON.stringify("
			"{wifi_ssid:s.value,wifi_psk:p.value,mqtt_host:m.value})}).then(r=>r.text()).then(alert)\">Save</button>"
			"</html>";
		return build_http(rsp, rsp_cap, rsp_len, "200 OK", "text/html", html);
	}

	if (starts_with(req, req_len, "GET /api/status ")) {
		const mcufw_ota_state_t *ota = mcufw_ota_state();
		const mcufw_state_t *st = mcufw_state();
		size_t p = 0u;
		s_put(body_buf, sizeof(body_buf), &p, "{\"fw\":\"0.8-phase4b.4\",\"wlan\":\"");
		s_put(body_buf, sizeof(body_buf), &p, mcufw_wlan_state_name(mcufw_wlan_get_state()));
		s_put(body_buf, sizeof(body_buf), &p, "\",\"ota\":\"");
		s_put(body_buf, sizeof(body_buf), &p, mcufw_ota_status_name(ota->status));
		s_put(body_buf, sizeof(body_buf), &p, "\",\"ota_err\":\"");
		s_put(body_buf, sizeof(body_buf), &p, ota->last_error);
		s_put(body_buf, sizeof(body_buf), &p, "\",\"power\":");
		s_dec(body_buf, sizeof(body_buf), &p, st->power);
		s_put(body_buf, sizeof(body_buf), &p, ",\"bright\":");
		s_dec(body_buf, sizeof(body_buf), &p, st->bright);
		s_put(body_buf, sizeof(body_buf), &p, ",\"ct\":");
		s_dec(body_buf, sizeof(body_buf), &p, st->ct);
		s_put(body_buf, sizeof(body_buf), &p, "}");
		return build_http(rsp, rsp_cap, rsp_len, "200 OK", "application/json", body_buf);
	}

	if (starts_with(req, req_len, "GET /api/config ")) {
		if (mcufw_devcfg_to_json(mcufw_devcfg(), body_buf, sizeof(body_buf)) < 0)
			return build_http(rsp, rsp_cap, rsp_len, "500 Internal Error",
			                  "application/json", "{\"error\":\"encode\"}");
		return build_http(rsp, rsp_cap, rsp_len, "200 OK", "application/json", body_buf);
	}

	if (starts_with(req, req_len, "POST /api/config ")) {
		if (!body || mcufw_devcfg_apply_json(body, blen) != 0)
			return build_http(rsp, rsp_cap, rsp_len, "400 Bad Request",
			                  "application/json", "{\"error\":\"invalid_config\"}");
		(void)mcufw_devcfg_to_json(mcufw_devcfg(), body_buf, sizeof(body_buf));
		return build_http(rsp, rsp_cap, rsp_len, "200 OK", "application/json", body_buf);
	}

	if (starts_with(req, req_len, "POST /api/ota ")) {
		char url[MCUFW_OTA_URL_MAX];
		char md5[MCUFW_OTA_MD5_HEX + 4u];
		url[0] = '\0';
		md5[0] = '\0';
		if (!body || json_str_val(body, blen, "app_url", url, sizeof(url)) != 0)
			return build_http(rsp, rsp_cap, rsp_len, "400 Bad Request",
			                  "application/json", "{\"error\":\"missing_app_url\"}");
		(void)json_str_val(body, blen, "file_md5", md5, sizeof(md5));
		if (mcufw_ota_queue(url, md5) != 0)
			return build_http(rsp, rsp_cap, rsp_len, "400 Bad Request",
			                  "application/json", "{\"error\":\"ota_rejected\"}");
		return build_http(rsp, rsp_cap, rsp_len, "202 Accepted",
		                  "application/json", "{\"status\":\"queued\"}");
	}

	if (starts_with(req, req_len, "POST /api/rollback ")) {
		mcufw_ota_request_rollback();
		return build_http(rsp, rsp_cap, rsp_len, "202 Accepted",
		                  "application/json", "{\"status\":\"rollback_scheduled\"}");
	}

	return build_http(rsp, rsp_cap, rsp_len, "404 Not Found",
	                  "application/json", "{\"error\":\"not_found\"}");
}
