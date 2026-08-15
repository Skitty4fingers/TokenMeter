#include "net/wifi_mgr.h"
#include "app_state.h"
#include "storage/config.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define STA_MAX_RETRY     3       /* consecutive quick retries before FALLBACK */
#define STA_RETRY_MS      30000   /* background retry period in FALLBACK */
#define AP_CHANNEL        1
#define AP_MAX_CONN       4

static esp_netif_t *s_sta_netif, *s_ap_netif;
static dns_server_handle_t s_dns;
static esp_timer_handle_t s_retry_tmr, s_rssi_tmr, s_linger_tmr, s_reconn_tmr;
static int s_retry;
static bool s_ap_up, s_online, s_started;
static char s_ap_ssid[33], s_ap_pass[17];
static wifi_state_cb_t s_cb;

static void notify(void) { if (s_cb) s_cb(); }

static void set_state(net_state_t st)
{
    app_state_lock();
    app_state_t *s = app_state_get();
    if (s->net != st) { s->net = st; app_state_touch(); }
    app_state_unlock();
    notify();
}

/* --- AP credentials: SSID from MAC, password random once, stored in NVS --- */
static void ap_creds_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "TokenMeter-%02X%02X", mac[4], mac[5]);

    nvs_handle_t h;
    size_t len = sizeof(s_ap_pass);
    bool have = false;
    if (nvs_open("tokenmeter", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_str(h, "appass", s_ap_pass, &len) == ESP_OK && strlen(s_ap_pass) >= 8) have = true;
        if (!have) {
            /* word-ish + 4 digits: easy to type from the LCD */
            static const char *words[] = {"kite","lark","moss","opal","reef","sage","tern","wren","fern","dune","iris","jade","lynx","mint","nova","pine"};
            uint32_t r = esp_random();
            snprintf(s_ap_pass, sizeof(s_ap_pass), "%s-%04lu", words[r & 15], (unsigned long)((r >> 4) % 10000));
            nvs_set_str(h, "appass", s_ap_pass);
            nvs_commit(h);
        }
        nvs_close(h);
    } else {
        strlcpy(s_ap_pass, "tokenmeter", sizeof(s_ap_pass));
    }
    app_state_lock();
    strlcpy(app_state_get()->ap_ssid, s_ap_ssid, sizeof(app_state_get()->ap_ssid));
    strlcpy(app_state_get()->ap_pass, s_ap_pass, sizeof(app_state_get()->ap_pass));
    app_state_unlock();
}

const char *wifi_mgr_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_mgr_ap_pass(void) { return s_ap_pass; }

/* --- softAP up/down --- */
static void ap_start(void)
{
    if (s_ap_up) return;
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (!s_dns) {
        dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dc);
    }
    s_ap_up = true;
    ESP_LOGI(TAG, "softAP %s up (pass %s)", s_ap_ssid, s_ap_pass);
}

static void ap_stop(void)
{
    if (!s_ap_up) return;
    if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_up = false;
    ESP_LOGI(TAG, "softAP down");
}

/* --- STA --- */
static void sta_connect(void)
{
    app_config_t *c = config_get();
    if (!config_has_wifi(c)) return;
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, c->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, c->wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = c->wifi_pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e != ESP_OK) { ESP_LOGW(TAG, "set_config: %s (connect in progress?)", esp_err_to_name(e)); return; }
    app_state_lock();
    strlcpy(app_state_get()->ssid, c->wifi_ssid, sizeof(app_state_get()->ssid));
    app_state_unlock();
    e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(e));
}

static void reconn_cb(void *arg) { if (!s_online) sta_connect(); }
static void reconnect_in(uint32_t ms)
{
    if (esp_timer_is_active(s_reconn_tmr)) esp_timer_stop(s_reconn_tmr);
    esp_timer_start_once(s_reconn_tmr, (uint64_t)ms * 1000);
}

static void retry_cb(void *arg) { if (!s_online) { ESP_LOGI(TAG, "background STA retry"); sta_connect(); } }
static void linger_cb(void *arg) { if (s_online) ap_stop(); }
static void ap_linger(uint32_t secs)
{
    if (esp_timer_is_active(s_linger_tmr)) esp_timer_stop(s_linger_tmr);
    esp_timer_start_once(s_linger_tmr, (uint64_t)secs * 1000000ULL);
}

static void rssi_cb(void *arg)
{
    if (!s_online) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        app_state_lock();
        app_state_get()->rssi = ap.rssi;
        app_state_unlock();
    }
}

static void enter_fallback(void)
{
    ap_start();
    set_state(NET_FALLBACK);
    if (!esp_timer_is_active(s_retry_tmr)) esp_timer_start_periodic(s_retry_tmr, (uint64_t)STA_RETRY_MS * 1000);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (config_has_wifi(config_get())) { set_state(NET_CONNECTING); sta_connect(); }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            bool was_online = s_online;
            s_online = false;
            app_state_lock();
            app_state_get()->ip[0] = 0; app_state_get()->rssi = 0; app_state_touch();
            app_state_unlock();
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", d ? d->reason : -1, s_retry);
            if (!config_has_wifi(config_get())) break;   /* forgotten — stay in PROVISION */
            if (was_online) { s_retry = 0; }
            if (s_retry < STA_MAX_RETRY) {
                s_retry++;
                set_state(NET_CONNECTING);
                reconnect_in(1500);          /* never block the event loop; never double-connect */
            } else {
                enter_fallback();
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "client joined hotspot");
            break;
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_online = true; s_retry = 0;
        app_state_lock();
        snprintf(app_state_get()->ip, sizeof(app_state_get()->ip), IPSTR, IP2STR(&e->ip_info.ip));
        app_state_touch();
        app_state_unlock();
        ESP_LOGI(TAG, "got IP %s", app_state_get()->ip);
        if (esp_timer_is_active(s_retry_tmr)) esp_timer_stop(s_retry_tmr);
        /* Leave the hotspot up a while: a phone on it is mid-wizard (10 min, or until
           /api/setup/done) or just watched us recover from fallback (60 s). */
        if (s_ap_up) {
            net_state_t prev; app_state_lock(); prev = app_state_get()->net; app_state_unlock();
            ap_linger(prev == NET_FALLBACK ? 60 : 1200);
        }
        set_state(NET_ONLINE);
        rssi_cb(NULL);
    }
}

void wifi_mgr_set_callback(wifi_state_cb_t cb) { s_cb = cb; }
bool wifi_mgr_is_online(void) { return s_online; }

void wifi_mgr_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ap_creds_init();
    const esp_timer_create_args_t a = { .callback = retry_cb, .name = "sta_retry" };
    esp_timer_create(&a, &s_retry_tmr);
    const esp_timer_create_args_t b = { .callback = rssi_cb, .name = "rssi" };
    esp_timer_create(&b, &s_rssi_tmr);
    const esp_timer_create_args_t l = { .callback = linger_cb, .name = "ap_linger" };
    esp_timer_create(&l, &s_linger_tmr);
    const esp_timer_create_args_t r = { .callback = reconn_cb, .name = "sta_reconn" };
    esp_timer_create(&r, &s_reconn_tmr);
    esp_timer_start_periodic(s_rssi_tmr, 10 * 1000 * 1000);
}

void wifi_mgr_start(void)
{
    if (config_has_wifi(config_get())) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        set_state(NET_CONNECTING);
    } else {
        ap_start();                 /* APSTA so scanning works during provisioning */
        set_state(NET_PROVISION);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    app_config_t *c = config_get();
    strlcpy(c->wifi_ssid, ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, pass ? pass : "", sizeof(c->wifi_pass));
    config_save(c);
    s_retry = 0;
    set_state(NET_CONNECTING);
    /* If associated, disconnect and let the DISCONNECTED handler reconnect with the new
       config; otherwise connect right away. (Calling connect twice yields "sta is connecting".)
       If the AP is up we stay APSTA so the phone keeps its session while we try. */
    if (s_online) { s_online = false; esp_wifi_disconnect(); }
    else { esp_wifi_disconnect(); reconnect_in(300); }
    return ESP_OK;
}

void wifi_mgr_forget(void)
{
    app_config_t *c = config_get();
    c->wifi_ssid[0] = 0; c->wifi_pass[0] = 0;
    config_save(c);
    s_online = false;
    if (esp_timer_is_active(s_retry_tmr)) esp_timer_stop(s_retry_tmr);
    if (esp_timer_is_active(s_reconn_tmr)) esp_timer_stop(s_reconn_tmr);
    esp_wifi_disconnect();
    ap_start();
    set_state(NET_PROVISION);
}

int wifi_mgr_scan_json(char *buf, size_t cap)
{
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) { snprintf(buf, cap, "[]"); ESP_LOGW(TAG, "scan: %s", esp_err_to_name(e)); return 0; }
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) { snprintf(buf, cap, "[]"); return 0; }
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t off = 0; int cnt = 0;
    off += snprintf(buf + off, cap - off, "[");
    for (int i = 0; i < n && off < cap - 96; i++) {
        /* dedupe same SSID (keep strongest; list is sorted by RSSI already) */
        bool dup = false;
        for (int j = 0; j < i; j++) if (!strcmp((char *)recs[j].ssid, (char *)recs[i].ssid)) { dup = true; break; }
        if (dup || !recs[i].ssid[0]) continue;
        /* escape quotes/backslashes in SSID */
        char esc[70]; size_t k = 0;
        for (const char *p = (char *)recs[i].ssid; *p && k < sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') esc[k++] = '\\';
            esc[k++] = *p;
        }
        esc[k] = 0;
        off += snprintf(buf + off, cap - off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                        cnt ? "," : "", esc, recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        cnt++;
    }
    snprintf(buf + off, cap - off, "]");
    free(recs);
    return cnt;
}

void wifi_mgr_setup_done(void)
{
    if (esp_timer_is_active(s_linger_tmr)) esp_timer_stop(s_linger_tmr);
    if (s_online) ap_stop();
}
