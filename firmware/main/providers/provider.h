#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    provider_id_t id;
    bool (*enabled)(void);                        /* credentials present in config */
    uint16_t (*interval_s)(void);                 /* configured poll interval */
    /* Fetch usage. Fills *out (ok/err/win/plan). Returns ESP_OK if out->ok. */
    esp_err_t (*fetch)(provider_usage_t *out);
    /* Optional: validate credentials passed in from the web UI without saving. */
} provider_t;

extern const provider_t provider_claude;
extern const provider_t provider_openai;

/* verification helpers used by the web UI "Verify" buttons */
esp_err_t claude_verify_token(const char *token, provider_usage_t *out);
/* access + refresh (+ expiry epoch, 0 if unknown) e.g. from ~/.claude/.credentials.json; writes into live config on success */
esp_err_t claude_verify_credentials(const char *access, const char *refresh, int64_t exp, provider_usage_t *out);
/* device's own login (PKCE, manual redirect): start → authorize URL; finish(pasted "code#state") → tokens in live config */
esp_err_t claude_oauth_start(char *url, size_t cap);
esp_err_t claude_oauth_finish(const char *pasted, provider_usage_t *out);
esp_err_t openai_verify_tokens(const char *access, const char *refresh, const char *account, provider_usage_t *out);

#ifdef __cplusplus
}
#endif
