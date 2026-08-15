#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

void wifi_mgr_init(void);          /* netifs, event handlers, AP creds */
void wifi_mgr_start(void);         /* PROVISION or CONNECTING based on config */
/* Save creds and (re)connect. Called from the web UI. */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass);
/* Blocking scan → JSON array string in buf. Returns number of APs. */
int wifi_mgr_scan_json(char *buf, size_t cap);
/* Forget creds → back to PROVISION (does not reboot). */
void wifi_mgr_forget(void);
/* Set by main: called when state changes (already holding app_state lock is NOT required). */
typedef void (*wifi_state_cb_t)(void);
void wifi_mgr_set_callback(wifi_state_cb_t cb);
/* Notified when the poller should wake (got IP). */
bool wifi_mgr_is_online(void);
/* Wizard finished: drop the hotspot now if STA is online. */
void wifi_mgr_setup_done(void);
const char *wifi_mgr_ap_ssid(void);
const char *wifi_mgr_ap_pass(void);

#ifdef __cplusplus
}
#endif
