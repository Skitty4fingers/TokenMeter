#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_VERSION 1

#define CFG_SSID_LEN     33
#define CFG_PASS_LEN     65
#define CFG_TOKEN_LEN    256   /* claude setup-token (sk-ant-oat01-…) is ~100 chars */
#define CFG_JWT_LEN      2048  /* openai access token is a JWT, ~1–1.5 KB */
#define CFG_RT_LEN       512
#define CFG_ACCT_LEN     64
#define CFG_TZ_LEN       48

typedef struct {
    uint32_t version;

    /* wifi */
    char wifi_ssid[CFG_SSID_LEN];
    char wifi_pass[CFG_PASS_LEN];

    /* claude — access token (sk-ant-oat01-…) + refresh token from ~/.claude/.credentials.json */
    char claude_token[CFG_TOKEN_LEN];

    /* openai / codex */
    char oa_access[CFG_JWT_LEN];
    char oa_refresh[CFG_RT_LEN];
    char oa_account[CFG_ACCT_LEN];
    int64_t oa_access_exp;   /* epoch seconds, 0 = unknown */

    /* display / behaviour */
    uint16_t poll_claude_s;
    uint16_t poll_openai_s;
    uint8_t  warn_pct;
    uint8_t  crit_pct;
    uint8_t  brightness;      /* 0..50 (%) */
    bool     night_dim;
    uint8_t  night_start_h;
    uint8_t  night_end_h;
    uint8_t  night_brightness;
    bool     led_enabled;
    bool     sd_log;
    uint8_t  rotation;        /* 0 = USB left, 1 = USB right */
    char     tz[CFG_TZ_LEN];  /* POSIX TZ string */

    /* --- appended in v1.1: loader zero-fills these when an older blob is found --- */
    char    claude_refresh[CFG_TOKEN_LEN];
    int64_t claude_access_exp;   /* epoch seconds, 0 = unknown */
    uint8_t led_night_off;       /* 0/1/255(unset→default on): LED dark during night hours */
    char    claude_scope[128];   /* OAuth scopes granted to the device's Claude login (sent on refresh) */
} app_config_t;

void config_defaults(app_config_t *c);
/* load from NVS; returns false (and fills defaults) if nothing stored */
bool config_load(app_config_t *c);
bool config_save(const app_config_t *c);
bool config_erase(void);

/* convenience */
static inline bool config_has_wifi(const app_config_t *c)   { return c->wifi_ssid[0] != 0; }
static inline bool config_has_claude(const app_config_t *c) { return c->claude_token[0] != 0; }
static inline bool config_has_openai(const app_config_t *c) { return c->oa_refresh[0] != 0 || c->oa_access[0] != 0; }

/* the live config singleton (guarded by app_state lock for writes) */
app_config_t *config_get(void);

#ifdef __cplusplus
}
#endif
