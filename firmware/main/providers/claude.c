/* Claude subscription usage — the endpoint Claude Code's /usage uses.
 * UNOFFICIAL and undocumented; may change without notice.
 *   - needs an OAuth token WITH the user:profile scope: that's Claude Code's login token
 *     (~/.claude/.credentials.json), NOT `claude setup-token` (inference-only → 403)
 *   - User-Agent must look like claude-code/x.y.z or you get long 429 lockouts
 *   - access tokens live ~8 h → we refresh with the refresh token (~28 d) on-device
 *   - poll no faster than ~180 s
 */
#include "providers/provider.h"
#include "providers/http_util.h"
#include "storage/config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "claude";
#define USAGE_URL "https://api.anthropic.com/api/oauth/usage"
#define TOKEN_URL "https://console.anthropic.com/v1/oauth/token"
#define CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"   /* Claude Code's public OAuth client */
#define UA        "claude-code/2.1.0"
#define BODY_CAP  4096

static bool en(void) { return config_has_claude(config_get()); }
static uint16_t ivl(void) { return config_get()->poll_claude_s; }

/* ---- refresh ---- */
static esp_err_t refresh_noSave(app_config_t *cfg, char *err, size_t errlen)
{
    if (!cfg->claude_refresh[0]) { snprintf(err, errlen, "token expired · re-paste credentials.json"); return ESP_FAIL; }
    ESP_LOGI(TAG, "refreshing access token");
    char *form = malloc(CFG_TOKEN_LEN + 160);
    if (!form) return ESP_ERR_NO_MEM;
    snprintf(form, CFG_TOKEN_LEN + 160, "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"%s\"}", cfg->claude_refresh, CLIENT_ID);
    const char *hdr[] = { "User-Agent", UA, "Accept", "application/json", NULL };
    char *body = malloc(BODY_CAP);
    if (!body) { free(form); return ESP_ERR_NO_MEM; }
    http_resp_t r = { .buf = body, .cap = BODY_CAP };
    esp_err_t e = http_request(TOKEN_URL, HTTP_METHOD_POST, hdr, form, "application/json", &r, 15000, 1024);
    free(form);
    if (e != ESP_OK) { snprintf(err, errlen, "refresh: %s", esp_err_to_name(e)); free(body); return e; }
    if (r.status != 200) {
        snprintf(err, errlen, "%d refresh rejected · re-paste credentials.json", r.status);
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
    strlcpy(cfg->claude_token, at->valuestring, sizeof(cfg->claude_token));
    if (cJSON_IsString(rt) && rt->valuestring[0]) strlcpy(cfg->claude_refresh, rt->valuestring, sizeof(cfg->claude_refresh));
    cfg->claude_access_exp = cJSON_IsNumber(ei) ? time(NULL) + (int64_t)ei->valuedouble : time(NULL) + 7 * 3600;
    cJSON_Delete(root);
    return ESP_OK;
}
static esp_err_t refresh_tokens(app_config_t *cfg, char *err, size_t errlen)
{
    esp_err_t e = refresh_noSave(cfg, err, errlen);
    if (e == ESP_OK) config_save(cfg);            /* refresh tokens may rotate — persist now */
    return e;
}

/* ---- usage ---- */
static void add_win(provider_usage_t *o, const char *label, double pct, const char *resets)
{
    if (o->nwin >= PROV_MAX_WIN) return;
    usage_window_t *w = &o->win[o->nwin++];
    w->valid = true;
    strlcpy(w->label, label, sizeof(w->label));
    w->pct = (float)pct;
    w->resets_at = resets ? parse_iso8601(resets) : 0;
}
static void set_win(provider_usage_t *o, const char *label, cJSON *node)
{
    if (!node || cJSON_IsNull(node)) return;
    cJSON *u = cJSON_GetObjectItem(node, "utilization");
    cJSON *r = cJSON_GetObjectItem(node, "resets_at");
    if (!cJSON_IsNumber(u)) return;
    add_win(o, label, u->valuedouble, cJSON_IsString(r) ? r->valuestring : NULL);
}

/* returns ESP_ERR_INVALID_STATE on 401/403-scope so the caller can refresh */
static esp_err_t get_usage(const char *token, provider_usage_t *o)
{
    char auth[CFG_TOKEN_LEN + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    const char *hdr[] = {
        "Authorization", auth,
        "anthropic-beta", "oauth-2025-04-20",
        "User-Agent", UA,
        "Accept", "application/json",
        NULL,
    };
    char *body = malloc(BODY_CAP);
    if (!body) return ESP_ERR_NO_MEM;
    http_resp_t r = { .buf = body, .cap = BODY_CAP };
    esp_err_t e = http_request(USAGE_URL, HTTP_METHOD_GET, hdr, NULL, NULL, &r, 15000, 1024);
    esp_err_t ret = ESP_FAIL;
    if (e != ESP_OK) { snprintf(o->err, sizeof(o->err), "network: %s", esp_err_to_name(e)); ret = e; goto out; }
    if (r.status == 401) { snprintf(o->err, sizeof(o->err), "401 token expired"); ret = ESP_ERR_INVALID_STATE; goto out; }
    if (r.status == 403) {
        if (strstr(body, "scope")) snprintf(o->err, sizeof(o->err), "403 token lacks user:profile scope · paste credentials.json, not setup-token");
        else snprintf(o->err, sizeof(o->err), "403 token rejected · re-paste credentials.json");
        goto out;
    }
    if (r.status == 429) { snprintf(o->err, sizeof(o->err), "429 rate limited · backing off"); goto out; }
    if (r.status != 200) { snprintf(o->err, sizeof(o->err), "HTTP %d", r.status); goto out; }

    cJSON *root = cJSON_Parse(body);
    if (!root) { snprintf(o->err, sizeof(o->err), "bad JSON (%u B)", (unsigned)r.len); goto out; }
    set_win(o, "5h",     cJSON_GetObjectItem(root, "five_hour"));
    set_win(o, "wk",     cJSON_GetObjectItem(root, "seven_day"));
    set_win(o, "opus",   cJSON_GetObjectItem(root, "seven_day_opus"));
    set_win(o, "sonnet", cJSON_GetObjectItem(root, "seven_day_sonnet"));
    /* per-model weekly limits live in limits[] as kind=weekly_scoped with scope.model.display_name */
    cJSON *lim = cJSON_GetObjectItem(root, "limits");
    cJSON *it;
    cJSON_ArrayForEach(it, lim) {
        cJSON *kind = cJSON_GetObjectItem(it, "kind");
        if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "weekly_scoped") != 0) continue;
        cJSON *pct = cJSON_GetObjectItem(it, "percent");
        cJSON *rs  = cJSON_GetObjectItem(it, "resets_at");
        cJSON *scope = cJSON_GetObjectItem(it, "scope");
        cJSON *model = scope ? cJSON_GetObjectItem(scope, "model") : NULL;
        cJSON *name  = model ? cJSON_GetObjectItem(model, "display_name") : NULL;
        if (!cJSON_IsNumber(pct) || !cJSON_IsString(name)) continue;
        char label[8]; size_t n = 0;
        for (const char *p = name->valuestring; *p && n < sizeof(label) - 1; p++) label[n++] = (char)tolower((unsigned char)*p);
        label[n] = 0;
        bool dup = false;
        for (int i = 0; i < o->nwin; i++) if (!strcmp(o->win[i].label, label)) dup = true;
        if (!dup) add_win(o, label, pct->valuedouble, cJSON_IsString(rs) ? rs->valuestring : NULL);
    }
    cJSON_Delete(root);
    if (o->nwin == 0) { snprintf(o->err, sizeof(o->err), "no usage windows in reply"); goto out; }
    o->ok = true;
    o->fetched_at = time(NULL);
    strlcpy(o->plan, "Claude", sizeof(o->plan));
    ESP_LOGI(TAG, "5h %.0f%% wk %.0f%% (%d windows)", o->win[0].pct, o->nwin > 1 ? o->win[1].pct : -1.0f, o->nwin);
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
    time_t now = time(NULL);
    if (cfg->claude_refresh[0] && cfg->claude_access_exp && cfg->claude_access_exp - now < 300) {
        esp_err_t e = refresh_tokens(cfg, o->err, sizeof(o->err));
        if (e != ESP_OK) return e;
    }
    esp_err_t e = get_usage(cfg->claude_token, o);
    if (e == ESP_ERR_INVALID_STATE) {
        e = refresh_tokens(cfg, o->err, sizeof(o->err));
        if (e != ESP_OK) return e;
        e = get_usage(cfg->claude_token, o);
        if (e == ESP_ERR_INVALID_STATE) snprintf(o->err, sizeof(o->err), "401 token rejected after refresh · re-paste credentials.json");
    }
    return e;
}

esp_err_t claude_verify_token(const char *token, provider_usage_t *out)
{
    return claude_verify_credentials(token, NULL, 0, out);
}

/* Verify (refreshing if needed) on a scratch config; copy into live config on success. Caller saves. */
esp_err_t claude_verify_credentials(const char *access, const char *refresh, int64_t exp, provider_usage_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    app_config_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, config_get(), sizeof(*tmp));
    strlcpy(tmp->claude_token,   access  ? access  : "", sizeof(tmp->claude_token));
    strlcpy(tmp->claude_refresh, refresh ? refresh : "", sizeof(tmp->claude_refresh));
    tmp->claude_access_exp = exp;
    esp_err_t e = get_usage(tmp->claude_token, out);
    if (e == ESP_ERR_INVALID_STATE) {
        e = refresh_noSave(tmp, out->err, sizeof(out->err));
        if (e == ESP_OK) e = get_usage(tmp->claude_token, out);
    }
    if (e == ESP_OK) {
        app_config_t *live = config_get();
        strlcpy(live->claude_token,   tmp->claude_token,   sizeof(live->claude_token));
        strlcpy(live->claude_refresh, tmp->claude_refresh, sizeof(live->claude_refresh));
        live->claude_access_exp = tmp->claude_access_exp;
    }
    free(tmp);
    return e;
}

const provider_t provider_claude = { .name = "claude", .id = PROV_CLAUDE, .enabled = en, .interval_s = ivl, .fetch = fetch };
