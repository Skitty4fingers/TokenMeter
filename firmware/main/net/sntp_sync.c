#include "net/sntp_sync.h"
#include "app_state.h"
#include <stdlib.h>
#include <time.h>
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_log.h"

static bool s_started;

static void on_sync(struct timeval *tv)
{
    ESP_LOGI("sntp", "time synced");
    app_state_lock();
    app_state_get()->time_synced = true;
    app_state_touch();
    app_state_unlock();
}

void sntp_sync_start(void)
{
    if (s_started) { esp_netif_sntp_start(); return; }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    s_started = true;
}

bool sntp_time_valid(void)
{
    time_t now = time(NULL);
    return now > 1700000000;   /* after Nov 2023 → not the 1970 default */
}

void sntp_apply_tz(const char *posix_tz)
{
    setenv("TZ", posix_tz && posix_tz[0] ? posix_tz : "UTC0", 1);
    tzset();
}
