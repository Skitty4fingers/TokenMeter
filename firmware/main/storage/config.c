#include "storage/config.h"
#include <string.h>
#include <stddef.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";
#define NS "tokenmeter"
#define KEY "cfg"

static app_config_t s_cfg;

app_config_t *config_get(void) { return &s_cfg; }

void config_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    c->poll_claude_s = 180;
    c->poll_openai_s = 120;
    c->warn_pct = 70;
    c->crit_pct = 90;
    c->brightness = 40;
    c->night_dim = false;
    c->night_start_h = 23;
    c->night_end_h = 7;
    c->night_brightness = 10;
    c->led_enabled = true;
    c->led_night_off = 1;
    c->sd_log = false;
    c->rotation = 0;
    strlcpy(c->tz, "UTC0", sizeof(c->tz));
}

bool config_load(app_config_t *c)
{
    config_defaults(c);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no namespace yet, defaults");
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, KEY, NULL, &len);          /* query stored size */
    if (err == ESP_OK && len > 0 && len <= sizeof(*c)) {
        /* older (shorter) blobs load into the prefix; new trailing fields stay at defaults (zero) */
        err = nvs_get_blob(h, KEY, c, &len);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h);
    if (err != ESP_OK || c->version != CFG_VERSION) {
        ESP_LOGW(TAG, "stored config missing/incompatible (%s, len %u, ver %lu) — defaults",
                 esp_err_to_name(err), (unsigned)len, (unsigned long)c->version);
        config_defaults(c);
        return false;
    }
    if (len < sizeof(*c)) {
        ESP_LOGI(TAG, "migrated config blob %u → %u bytes", (unsigned)len, (unsigned)sizeof(*c));
        if (len <= offsetof(app_config_t, led_night_off)) c->led_night_off = 1;   /* field didn't exist: default on */
    }
    /* clamp a few things so a bad save can't brick the display */
    if (c->brightness > 50) c->brightness = 50;
    if (c->poll_claude_s < 120) c->poll_claude_s = 120;
    if (c->poll_openai_s < 60) c->poll_openai_s = 60;
    if (c->crit_pct <= c->warn_pct) { c->warn_pct = 70; c->crit_pct = 90; }
    return true;
}

bool config_save(const app_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return false; }
    err = nvs_set_blob(h, KEY, c, sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "saved");
    return err == ESP_OK;
}

bool config_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
