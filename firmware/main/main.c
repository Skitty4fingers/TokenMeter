/* Token Meter — ESP32-C6-LCD-1.47
 * Boot → (PROVISION | CONNECTING) → ONLINE ⇄ FALLBACK. See docs/PLAN.md. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "app.h"
#include "app_state.h"
#include "storage/config.h"
#include "board/lcd.h"
#include "board/led.h"
#include "board/sdcard.h"
#include "net/wifi_mgr.h"
#include "net/webui.h"
#include "net/sntp_sync.h"
#include "providers/provider.h"
#include "ui/ui.h"

static const char *TAG = "main";
static EventGroupHandle_t s_ev;
#define EV_POKE BIT0

static const provider_t *PROVIDERS[PROV_COUNT] = { &provider_claude, &provider_openai };
static int s_fails[PROV_COUNT];

/* ---------- hooks used by other modules ---------- */
void poller_poke(void) { if (s_ev) xEventGroupSetBits(s_ev, EV_POKE); }

void app_reboot(void)
{
    ESP_LOGW(TAG, "reboot requested");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void app_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    config_erase();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void app_apply_settings(void)
{
    app_config_t *c = config_get();
    sntp_apply_tz(c->tz);
    led_set_enabled(c->led_enabled);
    lcd_set_brightness(c->brightness);
    if (lcd_lock(200)) { ui_request_refresh(); lcd_unlock(); }
}

/* ---------- wifi state → side effects ---------- */
static void on_net_change(void)
{
    net_state_t n;
    app_state_lock(); n = app_state_get()->net; app_state_unlock();
    if (n == NET_ONLINE) sntp_sync_start();
    poller_poke();
}

/* ---------- night schedule ---------- */
static bool is_night(const app_config_t *c, bool synced)
{
    if (!synced) return false;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int h = tm.tm_hour;
    return c->night_start_h <= c->night_end_h
         ? (h >= c->night_start_h && h < c->night_end_h)
         : (h >= c->night_start_h || h < c->night_end_h);
}

static void apply_brightness(const app_config_t *c, bool night)
{
    uint8_t b = (c->night_dim && night) ? c->night_brightness : c->brightness;
    static uint8_t last = 255;
    if (b != last) { lcd_set_brightness(b); last = b; }
}

/* ---------- LED policy ---------- */
static void apply_led(const app_state_t *st, const app_config_t *c, bool night)
{
    if (night && c->led_night_off && st->net == NET_ONLINE) { led_set(LED_OFF); return; }   /* dark at night (setup/fallback still signal) */
    switch (st->net) {
    case NET_PROVISION: led_set(LED_BLUE_PULSE); return;
    case NET_FALLBACK:  led_set(LED_AMBER_BLINK); return;
    case NET_BOOT: case NET_CONNECTING: led_set(LED_OFF); return;
    default: break;
    }
    int worst = -1;
    for (int i = 0; i < PROV_COUNT; i++) {
        const provider_usage_t *p = &st->prov[i];
        if (!p->enabled || !p->fetched_at) continue;
        for (int w = 0; w < p->nwin; w++) {
            int lvl = p->win[w].pct >= c->crit_pct ? 2 : p->win[w].pct >= c->warn_pct ? 1 : 0;
            if (lvl > worst) worst = lvl;
        }
    }
    led_set(worst == 2 ? LED_RED : worst == 1 ? LED_AMBER : worst == 0 ? LED_GREEN : LED_OFF);
}

/* ---------- SD logging ---------- */
static void log_usage(const char *name, const provider_usage_t *p)
{
    if (!config_get()->sd_log || !sdcard_mounted()) return;
    char line[128];
    for (int i = 0; i < p->nwin; i++) {
        snprintf(line, sizeof(line), "%lld,%s,%s,%.1f,%lld", (long long)p->fetched_at, name,
                 p->win[i].label, p->win[i].pct, (long long)p->win[i].resets_at);
        sdcard_append_csv(line);
    }
}

/* ---------- poll task ---------- */
static void poll_task(void *arg)
{
    static provider_usage_t fresh;
    for (;;) {
        xEventGroupWaitBits(s_ev, EV_POKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        const app_config_t *cfg = config_get();
        app_state_t *st = app_state_get();
        time_t now = time(NULL);
        bool online = wifi_mgr_is_online();
        bool synced = sntp_time_valid();

        /* keep enabled flags current (web UI may add/remove a provider any time) */
        app_state_lock();
        for (int i = 0; i < PROV_COUNT; i++) {
            bool en = PROVIDERS[i]->enabled();
            provider_usage_t *p = &st->prov[i];
            if (en != p->enabled) {
                memset(p, 0, sizeof(*p));
                p->enabled = en;
                p->next_poll = 0;
                s_fails[i] = 0;
                app_state_touch();
            }
        }
        app_state_unlock();

        if (online && synced) {
            for (int i = 0; i < PROV_COUNT; i++) {
                const provider_t *P = PROVIDERS[i];
                if (!P->enabled()) continue;
                app_state_lock();
                time_t next = st->prov[i].next_poll;
                app_state_unlock();
                if (now < next) continue;

                ESP_LOGI(TAG, "poll %s", P->name);
                esp_err_t e = P->fetch(&fresh);
                now = time(NULL);
                app_state_lock();
                provider_usage_t *p = &st->prov[i];
                if (!p->enabled) { app_state_unlock(); continue; }   /* removed while fetching */
                if (e == ESP_OK) {
                    s_fails[i] = 0;
                    memcpy(p, &fresh, sizeof(*p));
                    p->enabled = true;
                    p->next_poll = now + P->interval_s();
                } else {
                    /* keep old data, surface the error, back off exponentially (cap 30 min) */
                    if (s_fails[i] < 6) s_fails[i]++;
                    p->ok = false;
                    strlcpy(p->err, fresh.err, sizeof(p->err));
                    uint32_t delay = P->interval_s() << (s_fails[i] - 1);
                    if (delay > 1800) delay = 1800;
                    if (strstr(p->err, "429")) delay = delay < 900 ? 900 : delay;   /* 429 lockouts are long */
                    p->next_poll = now + delay;
                    ESP_LOGW(TAG, "%s: %s (retry in %lus)", P->name, p->err, (unsigned long)delay);
                }
                app_state_touch();
                app_state_unlock();
                if (e == ESP_OK) log_usage(P->name, &fresh);   /* SD I/O outside the lock */
            }
        }

        static app_state_t snap;
        app_state_snapshot(&snap);
        bool night = is_night(cfg, synced);
        apply_led(&snap, cfg, night);
        apply_brightness(cfg, night);
    }
}

/* ---------- boot ---------- */
static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set("tokenmeter");
    mdns_instance_name_set("Token Meter");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_state_init();
    s_ev = xEventGroupCreate();
    config_load(config_get());
    sntp_apply_tz(config_get()->tz);

    /* display first so the user sees something within ~1 s */
    lcd_init(config_get()->rotation);
    if (lcd_lock(1000)) { ui_init(); lcd_unlock(); }
    lcd_set_brightness(config_get()->brightness);
    led_init();
    led_set_enabled(config_get()->led_enabled);
    sdcard_mount();

    wifi_mgr_init();
    wifi_mgr_set_callback(on_net_change);
    wifi_mgr_start();
    webui_start();
    mdns_start();

    xTaskCreate(poll_task, "poll", 12288, NULL, 3, NULL);
    ESP_LOGI(TAG, "up. hotspot %s / %s", wifi_mgr_ap_ssid(), wifi_mgr_ap_pass());
}
