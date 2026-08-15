#include "net/webui.h"
#include "net/wifi_mgr.h"
#include "app.h"
#include "app_state.h"
#include "storage/config.h"
#include "providers/provider.h"
#include "board/sdcard.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "cJSON.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* ---------- helpers ---------- */
static esp_err_t send_json(httpd_req_t *req, cJSON *j, int status)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status == 502) httpd_resp_set_status(req, "502 Bad Gateway");
    esp_err_t e = httpd_resp_sendstr(req, s);
    free(s);
    return e;
}

static esp_err_t send_err(httpd_req_t *req, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(req, j, status);
}

/* read body (≤ cap-1), NUL-terminate. Returns NULL on error (already responded). */
static char *read_body(httpd_req_t *req, size_t cap)
{
    if (req->content_len >= cap) { send_err(req, 400, "body too large"); return NULL; }
    char *b = malloc(req->content_len + 1);
    if (!b) { httpd_resp_send_500(req); return NULL; }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) { free(b); httpd_resp_send_500(req); return NULL; }
        got += r;
    }
    b[got] = 0;
    return b;
}

static const char *js(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }

static cJSON *usage_json(const provider_usage_t *p)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", p->enabled);
    if (!p->enabled) return j;
    cJSON_AddBoolToObject(j, "ok", p->ok);
    cJSON_AddStringToObject(j, "error", p->err);
    cJSON_AddStringToObject(j, "plan", p->plan);
    cJSON_AddNumberToObject(j, "fetched_at", (double)p->fetched_at);
    cJSON_AddNumberToObject(j, "next_poll", (double)p->next_poll);
    cJSON *w = cJSON_AddArrayToObject(j, "windows");
    for (int i = 0; i < p->nwin; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "label", p->win[i].label);
        cJSON_AddNumberToObject(x, "pct", p->win[i].pct);
        cJSON_AddNumberToObject(x, "resets_at", (double)p->win[i].resets_at);
        cJSON_AddItemToArray(w, x);
    }
    return j;
}

/* ---------- handlers ---------- */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static const char *net_name(net_state_t n)
{
    switch (n) { case NET_BOOT: return "boot"; case NET_PROVISION: return "provision"; case NET_CONNECTING: return "connecting";
                 case NET_ONLINE: return "online"; case NET_FALLBACK: return "fallback"; default: return "?"; }
}

static esp_err_t h_status(httpd_req_t *req)
{
    app_state_t *st = malloc(sizeof(*st));
    if (!st) return httpd_resp_send_500(req);
    app_state_snapshot(st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "net", net_name(st->net));
    cJSON_AddStringToObject(j, "ssid", st->ssid);
    cJSON_AddStringToObject(j, "ip", st->ip);
    cJSON_AddNumberToObject(j, "rssi", st->rssi);
    cJSON_AddStringToObject(j, "ap_ssid", st->ap_ssid);
    cJSON_AddBoolToObject(j, "time_synced", st->time_synced);
    cJSON_AddNumberToObject(j, "now", (double)time(NULL));
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap_free", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(j, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    size_t tot, used; sdcard_stats(&tot, &used);
    cJSON *sd = cJSON_AddObjectToObject(j, "sd");
    cJSON_AddBoolToObject(sd, "mounted", sdcard_mounted());
    cJSON_AddNumberToObject(sd, "total_kb", tot);
    cJSON_AddNumberToObject(sd, "used_kb", used);
    cJSON_AddItemToObject(j, "claude", usage_json(&st->prov[PROV_CLAUDE]));
    cJSON_AddItemToObject(j, "openai", usage_json(&st->prov[PROV_OPENAI]));
    free(st);
    return send_json(req, j, 200);
}

static void mask(const char *tok, char *out, size_t n)
{
    size_t l = strlen(tok);
    if (!l) { out[0] = 0; return; }
    if (l <= 8) { snprintf(out, n, "…%s", tok + (l > 4 ? l - 4 : 0)); return; }
    snprintf(out, n, "%.8s…%s", tok, tok + l - 4);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    app_config_t *c = config_get();
    char m[32];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(j, "has_claude", config_has_claude(c));
    mask(c->claude_token, m, sizeof(m)); cJSON_AddStringToObject(j, "claude_masked", m);
    cJSON_AddBoolToObject(j, "claude_has_refresh", c->claude_refresh[0] != 0);
    cJSON_AddNumberToObject(j, "claude_access_exp", (double)c->claude_access_exp);
    cJSON_AddBoolToObject(j, "has_openai", config_has_openai(c));
    cJSON_AddStringToObject(j, "openai_account", c->oa_account);
    cJSON_AddNumberToObject(j, "openai_access_exp", (double)c->oa_access_exp);
    cJSON_AddNumberToObject(j, "poll_claude_s", c->poll_claude_s);
    cJSON_AddNumberToObject(j, "poll_openai_s", c->poll_openai_s);
    cJSON_AddNumberToObject(j, "warn_pct", c->warn_pct);
    cJSON_AddNumberToObject(j, "crit_pct", c->crit_pct);
    cJSON_AddNumberToObject(j, "brightness", c->brightness);
    cJSON_AddBoolToObject(j, "night_dim", c->night_dim);
    cJSON_AddNumberToObject(j, "night_start_h", c->night_start_h);
    cJSON_AddNumberToObject(j, "night_end_h", c->night_end_h);
    cJSON_AddNumberToObject(j, "night_brightness", c->night_brightness);
    cJSON_AddBoolToObject(j, "led_enabled", c->led_enabled);
    cJSON_AddBoolToObject(j, "led_night_off", c->led_night_off == 1);
    cJSON_AddBoolToObject(j, "sd_log", c->sd_log);
    cJSON_AddNumberToObject(j, "rotation", c->rotation);
    cJSON_AddStringToObject(j, "tz", c->tz);
    return send_json(req, j, 200);
}

#define JN(o,k,dst,lo,hi) do { cJSON *_v = cJSON_GetObjectItem(o,k); if (cJSON_IsNumber(_v)) { int _x=(int)_v->valuedouble; if(_x<lo)_x=lo; if(_x>hi)_x=hi; dst=_x; } } while(0)
#define JB(o,k,dst)       do { cJSON *_v = cJSON_GetObjectItem(o,k); if (cJSON_IsBool(_v)) dst = cJSON_IsTrue(_v); } while(0)

static esp_err_t h_config_post(httpd_req_t *req)
{
    char *b = read_body(req, 2048); if (!b) return ESP_OK;
    cJSON *o = cJSON_Parse(b); free(b);
    if (!o) return send_err(req, 400, "bad JSON");
    app_config_t *c = config_get();
    JN(o, "poll_claude_s", c->poll_claude_s, 120, 3600);
    JN(o, "poll_openai_s", c->poll_openai_s, 60, 3600);
    JN(o, "warn_pct", c->warn_pct, 1, 99);
    JN(o, "crit_pct", c->crit_pct, 2, 100);
    if (c->crit_pct <= c->warn_pct) c->crit_pct = c->warn_pct + 1;
    JN(o, "brightness", c->brightness, 1, 50);
    JB(o, "night_dim", c->night_dim);
    JN(o, "night_start_h", c->night_start_h, 0, 23);
    JN(o, "night_end_h", c->night_end_h, 0, 23);
    JN(o, "night_brightness", c->night_brightness, 0, 50);
    JB(o, "led_enabled", c->led_enabled);
    { cJSON *_v = cJSON_GetObjectItem(o, "led_night_off"); if (cJSON_IsBool(_v)) c->led_night_off = cJSON_IsTrue(_v) ? 1 : 0; }
    JB(o, "sd_log", c->sd_log);
    JN(o, "rotation", c->rotation, 0, 1);
    const char *tz = js(o, "tz"); if (tz) strlcpy(c->tz, tz, sizeof(c->tz));
    cJSON_Delete(o);
    config_save(c);
    app_apply_settings();
    poller_poke();
    return h_config_get(req);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(req);
    wifi_mgr_scan_json(buf, 2048);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char *b = read_body(req, 512); if (!b) return ESP_OK;
    cJSON *o = cJSON_Parse(b); free(b);
    if (!o) return send_err(req, 400, "bad JSON");
    const char *ssid = js(o, "ssid"), *pass = js(o, "pass");
    if (!ssid || !ssid[0]) { cJSON_Delete(o); return send_err(req, 400, "ssid required"); }
    esp_err_t e = wifi_mgr_set_credentials(ssid, pass ? pass : "");
    cJSON_Delete(o);
    if (e != ESP_OK) return send_err(req, 400, "could not apply");
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "hint", "connecting — poll /api/status for ip");
    return send_json(req, j, 200);
}

static esp_err_t h_wifi_delete(httpd_req_t *req)
{
    wifi_mgr_forget();
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j, 200);
}

/* Accepts {"token": "<access token>"} or {"credjson": "<contents of ~/.claude/.credentials.json>"} */
static esp_err_t h_claude_post(httpd_req_t *req)
{
    char *b = read_body(req, 4096); if (!b) return ESP_OK;
    cJSON *o = cJSON_Parse(b); free(b);
    if (!o) return send_err(req, 400, "bad JSON");
    const char *access = js(o, "token"), *refresh = NULL;
    int64_t exp = 0;
    cJSON *inner = NULL;
    const char *cj = js(o, "credjson");
    if (cj) {
        inner = cJSON_Parse(cj);
        if (!inner) { cJSON_Delete(o); return send_err(req, 400, "credentials.json is not valid JSON"); }
        cJSON *c = cJSON_GetObjectItem(inner, "claudeAiOauth");
        if (!c) c = inner;
        access = js(c, "accessToken"); refresh = js(c, "refreshToken");
        cJSON *e = cJSON_GetObjectItem(c, "expiresAt");
        if (cJSON_IsNumber(e)) exp = (int64_t)(e->valuedouble / 1000.0);   /* ms → s */
    } else if (access && access[0] == '{') {
        /* user pasted the whole file into the token box — be forgiving */
        inner = cJSON_Parse(access);
        cJSON *c = inner ? cJSON_GetObjectItem(inner, "claudeAiOauth") : NULL;
        if (c) { access = js(c, "accessToken"); refresh = js(c, "refreshToken");
                 cJSON *e = cJSON_GetObjectItem(c, "expiresAt"); if (cJSON_IsNumber(e)) exp = (int64_t)(e->valuedouble / 1000.0); }
    }
    if (!access || strlen(access) < 20 || strlen(access) >= CFG_TOKEN_LEN) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return send_err(req, 400, "no usable access token found"); }
    if (refresh && strlen(refresh) >= CFG_TOKEN_LEN) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return send_err(req, 400, "refresh token too long"); }
    provider_usage_t *u = calloc(1, sizeof(*u));
    if (!u) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return httpd_resp_send_500(req); }
    esp_err_t e = claude_verify_credentials(access, refresh, exp, u);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    if (e == ESP_OK) {
        config_save(config_get());
        cJSON_AddItemToObject(j, "usage", usage_json(u));
        cJSON_AddBoolToObject(j, "has_refresh", refresh && refresh[0]);
        poller_poke();
    } else {
        cJSON_AddStringToObject(j, "error", u->err);
    }
    free(u); if (inner) cJSON_Delete(inner); cJSON_Delete(o);
    return send_json(req, j, e == ESP_OK ? 200 : 502);
}

static esp_err_t h_claude_delete(httpd_req_t *req)
{
    app_config_t *c = config_get();
    c->claude_token[0] = 0; c->claude_refresh[0] = 0; c->claude_access_exp = 0; config_save(c); poller_poke();
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j, 200);
}

/* Accepts either {"authjson": "<contents of ~/.codex/auth.json>"} or {"access","refresh","account"} */
static esp_err_t h_openai_post(httpd_req_t *req)
{
    char *b = read_body(req, 12 * 1024); if (!b) return ESP_OK;
    cJSON *o = cJSON_Parse(b); free(b);
    if (!o) return send_err(req, 400, "bad JSON");
    const char *access = js(o, "access"), *refresh = js(o, "refresh"), *account = js(o, "account");
    cJSON *inner = NULL;
    const char *aj = js(o, "authjson");
    if (aj) {
        inner = cJSON_Parse(aj);
        if (!inner) { cJSON_Delete(o); return send_err(req, 400, "auth.json is not valid JSON"); }
        cJSON *t = cJSON_GetObjectItem(inner, "tokens");
        if (t) { access = js(t, "access_token"); refresh = js(t, "refresh_token"); account = js(t, "account_id"); }
    }
    if (!access || !access[0]) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return send_err(req, 400, "no access_token found"); }
    if (strlen(access) >= CFG_JWT_LEN) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return send_err(req, 400, "access_token too long"); }
    if (refresh && strlen(refresh) >= CFG_RT_LEN) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return send_err(req, 400, "refresh_token too long"); }

    provider_usage_t *u = calloc(1, sizeof(*u));
    if (!u) { if (inner) cJSON_Delete(inner); cJSON_Delete(o); return httpd_resp_send_500(req); }
    esp_err_t e = openai_verify_tokens(access, refresh, account, u);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    if (e == ESP_OK) {
        config_save(config_get());          /* verify already copied tokens into live config */
        cJSON_AddItemToObject(j, "usage", usage_json(u));
        poller_poke();
    } else {
        cJSON_AddStringToObject(j, "error", u->err);
    }
    free(u); if (inner) cJSON_Delete(inner); cJSON_Delete(o);
    return send_json(req, j, e == ESP_OK ? 200 : 502);
}

static esp_err_t h_openai_delete(httpd_req_t *req)
{
    app_config_t *c = config_get();
    c->oa_access[0] = 0; c->oa_refresh[0] = 0; c->oa_account[0] = 0; c->oa_access_exp = 0;
    config_save(c); poller_poke();
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j, 200);
}

static esp_err_t h_setup_done(httpd_req_t *req)
{
    wifi_mgr_setup_done();
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j, 200);
}

static esp_err_t h_reset(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    send_json(req, j, 200);
    app_factory_reset();
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", true);
    send_json(req, j, 200);
    app_reboot();
    return ESP_OK;
}

/* Captive portal: anything unknown → redirect to our root. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    char host[64] = "192.168.4.1";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char loc[96];
    snprintf(loc, sizeof(loc), "http://%s/", host);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webui_start(void)
{
    if (s_srv) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    ESP_ERROR_CHECK(httpd_start(&s_srv, &cfg));

    const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = h_index },
        { .uri = "/index.html",   .method = HTTP_GET,    .handler = h_index },
        { .uri = "/api/status",   .method = HTTP_GET,    .handler = h_status },
        { .uri = "/api/config",   .method = HTTP_GET,    .handler = h_config_get },
        { .uri = "/api/config",   .method = HTTP_POST,   .handler = h_config_post },
        { .uri = "/api/scan",     .method = HTTP_GET,    .handler = h_scan },
        { .uri = "/api/wifi",     .method = HTTP_POST,   .handler = h_wifi_post },
        { .uri = "/api/wifi",     .method = HTTP_DELETE, .handler = h_wifi_delete },
        { .uri = "/api/claude",   .method = HTTP_POST,   .handler = h_claude_post },
        { .uri = "/api/claude",   .method = HTTP_DELETE, .handler = h_claude_delete },
        { .uri = "/api/openai",   .method = HTTP_POST,   .handler = h_openai_post },
        { .uri = "/api/openai",   .method = HTTP_DELETE, .handler = h_openai_delete },
        { .uri = "/api/setup/done", .method = HTTP_POST, .handler = h_setup_done },
        { .uri = "/api/reset",    .method = HTTP_POST,   .handler = h_reset },
        { .uri = "/api/reboot",   .method = HTTP_POST,   .handler = h_reboot },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) httpd_register_uri_handler(s_srv, &routes[i]);
    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "web UI up");
}
