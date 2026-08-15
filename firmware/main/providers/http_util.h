#pragma once
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  *buf;     /* caller-provided */
    size_t cap;
    size_t len;
    int    status;
} http_resp_t;

/* One-shot HTTPS request with the ESP cert bundle. headers = {"K","V",...,NULL}. */
esp_err_t http_request(const char *url, esp_http_client_method_t method,
                       const char **headers, const char *body, const char *content_type,
                       http_resp_t *resp, int timeout_ms, int tx_buf);

/* "2026-04-11T07:00:00.528743+00:00" / "...Z" → epoch (UTC). 0 on failure. */
time_t parse_iso8601(const char *s);

/* Decode a JWT payload claim (string or number-as-string) into out. Returns true on success. */
bool jwt_get_claim(const char *jwt, const char *path /* "exp" or "https://api.openai.com/auth.chatgpt_account_id" */,
                   char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
