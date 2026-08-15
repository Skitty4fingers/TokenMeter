#pragma once
#include <stdbool.h>
#include "lvgl.h"
lv_obj_t *ui_provision_create(void);
/* offline=true → "WiFi unreachable" wording instead of first-boot "setup mode" */
void ui_provision_set(const char *ssid, const char *pass, bool offline);
