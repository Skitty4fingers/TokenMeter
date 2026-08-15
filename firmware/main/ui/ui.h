#pragma once
#include "lvgl.h"
#include "app_state.h"
#include "storage/config.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Build all screens; starts a 1 s LVGL timer that pulls app_state and redraws. Call under lcd_lock. */
void ui_init(void);
/* Force an immediate refresh on the next tick (cheap; the tick already diffs seq). */
void ui_request_refresh(void);
#ifdef __cplusplus
}
#endif
