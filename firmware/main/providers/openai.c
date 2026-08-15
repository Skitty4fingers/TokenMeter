/* ChatGPT / Codex subscription usage — the endpoint Codex CLI's /status polls.
 * UNOFFICIAL. Sits behind Cloudflare, which fingerprints TLS; an ESP32 from a
 * residential IP passes today, but 403 is a real possibility → surfaced as an error row.
 * Access tokens are short-lived JWTs; refresh tokens ROTATE, so we persist the new one
 * to NVS immediately after every refresh.
 */
#include "providers/provider.h"
#include "providers/http_util.h"
#include "storage/config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "openai";
#define USAGE_URL "https://chatgpt.com/backend-api/wham/usage"
#define TOKEN_URL "https://auth.openai.com/oauth/token"
#define CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define UA        "codex_cli_rs/0.40.0"

static bool en(void) { return config_has_openai(config_get()); }
static uint16_t ivl(void) { return config_get()->poll_openai_s; }

/* ---- token handling ---- */
static int64_t jwt_exp(const char *jwt)
{
    char v[24];
    return jwt_get_claim(jwt, "exp", v, sizeof(v)) ? atoll(v) : 0;
}

static bool jwt_account(const char *jwt, char *out, size_t n)
{
    return jwt_get_claim(jwt, "https://api.openai.com/auth.chatgpt_account_id", out, n);
}

/* refresh into cfg (in place). Does NOT save. */
static esp_err_t refresh_tokens_noSave(app_config_t *cfg, char *err, size_t errlen)
{
    if (!cfg->oa_refresh[0]) { snprintf(err, errlen, "401 no refresh token · re-add in web UI"); return ESP_FAIL; }
    ESP_LOGI(TAG, "refreshing access token");
    char *form = malloc(CFG_RT_LEN + 128);
    if (!form) return ESP_ERR_NO_MEM;
    snprintf(form, CFG_RT_LEN + 128, "grant_type=refresh_token&refresh_token=%s&client_id=%s", cfg->oa_refresh, CLIENT_ID);
    const char *hdr[] = { "User-Agent", UA, "Accept", "application/json", NULL };
    char *body = malloc(CFG_JWT_LEN + CFG_RT_LEN + 1024);
    if (!body) { free(form); return ESP_ERR_NO_MEM; }
    http_resp_t r = { .buf = body, .cap = CFG_JWT_LEN + CFG_RT_LEN + 1024 };
    esp_err_t e = http_request(TOKEN_URL, HTTP_METHOD_POST, hdr, form, "application/x-www-form-urlencoded", &r, 15000, 2048);
    free(form);
    if (e != ESP_OK) { snprintf(err, errlen, "refresh: %s", esp_err_to_name(e)); free(body); return e; }
    if (r.status != 200) {
        snprintf(err, errlen, "%d refresh rejected · re-add auth.json", r.status);
        ESP_LOGW(TAG, "refresh %d: %.200s", r.status, body);
        free(body); return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { snprintf(err, errlen, "refresh: bad JSON"); return ESP_FAIL; }
    cJSON *at = cJSON_GetObjectItem(root, "access_token");
    cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    cJSON *ei = cJSON_GetObjectItem(root, "expires_in");
    if (!cJSON_IsString(at)) { cJSON_Delete(root); snprintf(err, errlen, "refresh: no access_token"); return ESP_FAIL; }
    strlcpy(cfg->oa_access, at->valuestring, sizeof(cfg->oa_access));
    if (cJSON_IsString(rt) && rt->valuestring[0]) strlcpy(cfg->oa_refresh, rt->valuestring, sizeof(cfg->oa_refresh));
    int64_t exp = jwt_exp(cfg->oa_access);
    if (!exp && cJSON_IsNumber(ei)) exp = time(NULL) + (int64_t)ei->valuedouble;
    cfg->oa_access_exp = exp;
    if (!cfg->oa_account[0]) jwt_account(cfg->oa_access, cfg->oa_account, sizeof(cfg->oa_account));
    cJSON_Delete(root);
    return ESP_OK;
}

/* refresh + persist (refresh tokens rotate — a lost new token strands the login) */
static esp_err_t refresh_tokens(app_config_t *cfg, char *err, size_t errlen)
{
    esp_err_t e = refresh_tokens_noSave(cfg, err, errlen);
    if (e == ESP_OK) config_save(cfg);
    return e;
}

/* ---- usage parsing ---- */
static void set_win(provider_usage_t *o, const char *label, cJSON *w)
{
    if (!w || cJSON_IsNull(w) || o->nwin >= PROV_MAX_WIN) return;
    cJSON *u = cJSON_GetObjectItem(w, "used_percent");
    if (!cJSON_IsNumber(u)) return;
    usage_window_t *x = &o->win[o->nwin++];
    x->valid = true; x->pct = (float)u->valuedouble;
    /* label by window length, not position: a fresh account only has the weekly window as "primary" */
    cJSON *lw = cJSON_GetObjectItem(w, "limit_window_seconds");
    if (cJSON_IsNumber(lw)) {
        long secs = (long)lw->valuedouble;
        if (secs == 18000) strlcpy(x->label, "5h", sizeof(x->label));
        else if (secs == 604800) strlcpy(x->label, "wk", sizeof(x->label));
        else if (secs % 86400 == 0) snprintf(x->label, sizeof(x->label), "%ldd", secs / 86400);
        else snprintf(x->label, sizeof(x->label), "%ldh", secs / 3600);
    } else strlcpy(x->label, label, sizeof(x->label));
    cJSON *ra = cJSON_GetObjectItem(w, "reset_at");
    cJSON *rs = cJSON_GetObjectItem(w, "reset_after_seconds");
    if (cJSON_IsNumber(ra)) x->resets_at = (time_t)ra->valuedouble;
    else if (cJSON_IsString(ra)) x->resets_at = parse_iso8601(ra->valuestring);
    else if (cJSON_IsNumber(rs)) x->resets_at = time(NULL) + (time_t)rs->valuedouble;
}

static esp_err_t get_usage(const char *access, const char *account, provider_usage_t *o)
{
    char *auth = malloc(CFG_JWT_LEN + 8);
    if (!auth) return ESP_ERR_NO_MEM;
    snprintf(auth, CFG_JWT_LEN + 8, "Bearer %s", access);
    const char *hdr[] = {
        "Authorization", auth,
        "ChatGPT-Account-ID", account,
        "User-Agent", UA,
        "Accept", "application/json",
        NULL,
    };
    const size_t cap = 3072;
    char *body = malloc(cap);
    if (!body) { free(auth); return ESP_ERR_NO_MEM; }
    http_resp_t r = { .buf = body, .cap = cap };
    esp_err_t e = http_request(USAGE_URL, HTTP_METHOD_GET, hdr, NULL, NULL, &r, 15000, CFG_JWT_LEN + 512);
    free(auth);
    esp_err_t ret = ESP_FAIL;
    if (e != ESP_OK) { snprintf(o->err, sizeof(o->err), "network: %s", esp_err_to_name(e)); ret = e; goto out; }
    if (r.status == 401) { snprintf(o->err, sizeof(o->err), "401"); ret = ESP_ERR_INVALID_STATE; goto out; }   /* caller may refresh */
    if (r.status == 403) { snprintf(o->err, sizeof(o->err), "403 blocked (Cloudflare) · retrying"); goto out; }
    if (r.status == 429) { snprintf(o->err, sizeof(o->err), "429 rate limited · backing off"); goto out; }
    if (r.status != 200) { snprintf(o->err, sizeof(o->err), "HTTP %d", r.status); goto out; }

    cJSON *root = cJSON_Parse(body);
    if (!root) { snprintf(o->err, sizeof(o->err), "bad JSON"); goto out; }
    cJSON *rl = cJSON_GetObjectItem(root, "rate_limit");
    if (!rl) rl = cJSON_GetObjectItem(root, "rate_limits");   /* older shape */
    if (rl) {
        cJSON *p = cJSON_GetObjectItem(rl, "primary_window");   if (!p) p = cJSON_GetObjectItem(rl, "primary");
        cJSON *s = cJSON_GetObjectItem(rl, "secondary_window"); if (!s) s = cJSON_GetObjectItem(rl, "secondary");
        set_win(o, "5h", p);
        set_win(o, "wk", s);
    }
    cJSON *plan = cJSON_GetObjectItem(root, "plan_type");
    if (cJSON_IsString(plan)) {
        /* "plus" → "Plus" */
        strlcpy(o->plan, plan->valuestring, sizeof(o->plan));
        if (o->plan[0] >= 'a' && o->plan[0] <= 'z') o->plan[0] -= 32;
    } else strlcpy(o->plan, "ChatGPT", sizeof(o->plan));
    cJSON_Delete(root);
    if (o->nwin == 0) { snprintf(o->err, sizeof(o->err), "no rate_limit in reply"); goto out; }
    o->ok = true;
    o->fetched_at = time(NULL);
    ESP_LOGI(TAG, "5h %.0f%% wk %.0f%%", o->win[0].pct, o->nwin > 1 ? o->win[1].pct : -1.0f);
    ret = ESP_OK;
out:
    free(body);
    return ret;
}

static esp_err_t fetch(provider_usage_t *o)
{
    app_config_t *cfg = config_get();
    memset(o, 0, sizeof(*o));
    o->enabled = true;

    if (!cfg->oa_account[0] && cfg->oa_access[0]) jwt_account(cfg->oa_access, cfg->oa_account, sizeof(cfg->oa_account));
    if (!cfg->oa_access_exp && cfg->oa_access[0]) cfg->oa_access_exp = jwt_exp(cfg->oa_access);

    time_t now = time(NULL);
    bool need_refresh = !cfg->oa_access[0] || (cfg->oa_access_exp && cfg->oa_access_exp - now < 120);
    if (need_refresh) {
        esp_err_t e = refresh_tokens(cfg, o->err, sizeof(o->err));
        if (e != ESP_OK) return e;
    }
    esp_err_t e = get_usage(cfg->oa_access, cfg->oa_account, o);
    if (e == ESP_ERR_INVALID_STATE) {           /* 401 → one refresh + retry */
        e = refresh_tokens(cfg, o->err, sizeof(o->err));
        if (e != ESP_OK) return e;
        e = get_usage(cfg->oa_access, cfg->oa_account, o);
        if (e == ESP_ERR_INVALID_STATE) snprintf(o->err, sizeof(o->err), "401 token expired · re-add in web UI");
    }
    return e;
}

esp_err_t openai_verify_tokens(const char *access, const char *refresh, const char *account, provider_usage_t *out)
{
    /* Verify against a scratch copy of the config; on success copy the (possibly refreshed)
       tokens into the live config. Caller saves. */
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    app_config_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, config_get(), sizeof(*tmp));
    strlcpy(tmp->oa_access,  access  ? access  : "", sizeof(tmp->oa_access));
    strlcpy(tmp->oa_refresh, refresh ? refresh : "", sizeof(tmp->oa_refresh));
    tmp->oa_account[0] = 0;
    if (account && account[0]) strlcpy(tmp->oa_account, account, sizeof(tmp->oa_account));
    else if (!jwt_account(tmp->oa_access, tmp->oa_account, sizeof(tmp->oa_account))) {
        snprintf(out->err, sizeof(out->err), "cannot read account_id from token");
        free(tmp); return ESP_FAIL;
    }
    tmp->oa_access_exp = jwt_exp(tmp->oa_access);

    esp_err_t e = get_usage(tmp->oa_access, tmp->oa_account, out);
    if (e == ESP_ERR_INVALID_STATE) {              /* pasted access token already stale → refresh */
        e = refresh_tokens_noSave(tmp, out->err, sizeof(out->err));
        if (e == ESP_OK) e = get_usage(tmp->oa_access, tmp->oa_account, out);
        if (e == ESP_ERR_INVALID_STATE) snprintf(out->err, sizeof(out->err), "401 token rejected after refresh");
    }
    if (e == ESP_OK) {
        app_config_t *live = config_get();
        strlcpy(live->oa_access,  tmp->oa_access,  sizeof(live->oa_access));
        strlcpy(live->oa_refresh, tmp->oa_refresh, sizeof(live->oa_refresh));
        strlcpy(live->oa_account, tmp->oa_account, sizeof(live->oa_account));
        live->oa_access_exp = tmp->oa_access_exp;
    }
    free(tmp);
    return e;
}

const provider_t provider_openai = { .name = "openai", .id = PROV_OPENAI, .enabled = en, .interval_s = ivl, .fetch = fetch };
