#include "providers/http_util.h"
#include <string.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "http";

static esp_err_t evt(esp_http_client_event_t *e)
{
    http_resp_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && r && r->buf) {
        size_t n = e->data_len;
        if (r->len + n >= r->cap) n = r->cap - 1 - r->len;   /* truncate, keep NUL */
        if (n) { memcpy(r->buf + r->len, e->data, n); r->len += n; r->buf[r->len] = 0; }
    }
    return ESP_OK;
}

esp_err_t http_request(const char *url, esp_http_client_method_t method,
                       const char **headers, const char *body, const char *content_type,
                       http_resp_t *resp, int timeout_ms, int tx_buf)
{
    resp->len = 0; resp->status = 0;
    if (resp->buf && resp->cap) resp->buf[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = evt,
        .user_data = resp,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = tx_buf > 0 ? tx_buf : 1024,
        .disable_auto_redirect = false,
        .max_redirection_count = 2,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    for (const char **h = headers; h && h[0]; h += 2) esp_http_client_set_header(c, h[0], h[1]);
    if (body) {
        if (content_type) esp_http_client_set_header(c, "Content-Type", content_type);
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) resp->status = esp_http_client_get_status_code(c);
    else ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return err;
}

time_t parse_iso8601(const char *s)
{
    if (!s || strlen(s) < 19) return 0;
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    if (M < 1 || M > 12 || D < 1 || D > 31) return 0;
    /* skip fraction */
    const char *p = s + 19;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    long off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1) off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
    }
    /* timegm equivalent (mktime would apply the local TZ) */
    static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    long y = Y, days = 0;
    days = (y - 1970) * 365 + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
    days += cum[M - 1] + (D - 1);
    bool leap = (Y % 4 == 0 && Y % 100 != 0) || (Y % 400 == 0);
    if (leap && M > 2) days += 1;
    time_t t = days * 86400 + h * 3600 + m * 60 + sec;
    return t - off;
}

/* base64url → bytes; returns length or -1 */
static int b64url_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    char *tmp = malloc(inlen + 4);
    if (!tmp) return -1;
    size_t n = 0;
    for (size_t i = 0; i < inlen; i++) {
        char c = in[i];
        if (c == '-') c = '+'; else if (c == '_') c = '/';
        tmp[n++] = c;
    }
    while (n % 4) tmp[n++] = '=';
    size_t olen = 0;
    int rc = mbedtls_base64_decode(out, outcap, &olen, (unsigned char *)tmp, n);
    free(tmp);
    return rc == 0 ? (int)olen : -1;
}

bool jwt_get_claim(const char *jwt, const char *path, char *out, size_t out_len)
{
    if (!jwt) return false;
    const char *p1 = strchr(jwt, '.');
    if (!p1) return false;
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2) return false;
    size_t plen = p2 - (p1 + 1);
    size_t cap = plen + 4;
    unsigned char *payload = malloc(cap + 1);
    if (!payload) return false;
    int n = b64url_decode(p1 + 1, plen, payload, cap);
    if (n < 0) { free(payload); return false; }
    payload[n] = 0;
    cJSON *root = cJSON_ParseWithLength((char *)payload, n);
    free(payload);
    if (!root) return false;

    /* path: "a.b" — but claim names may contain dots (URLs). We only support: exact top-level key,
       or "<toplevel>.<child>" where toplevel is tried as the longest prefix that exists. */
    bool ok = false;
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, path);
    if (!node) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            char top[96];
            size_t tl = dot - path;
            if (tl < sizeof(top)) {
                memcpy(top, path, tl); top[tl] = 0;
                cJSON *t = cJSON_GetObjectItemCaseSensitive(root, top);
                if (t) node = cJSON_GetObjectItemCaseSensitive(t, dot + 1);
            }
        }
    }
    if (node) {
        if (cJSON_IsString(node)) { strlcpy(out, node->valuestring, out_len); ok = true; }
        else if (cJSON_IsNumber(node)) { snprintf(out, out_len, "%.0f", node->valuedouble); ok = true; }
    }
    cJSON_Delete(root);
    return ok;
}
