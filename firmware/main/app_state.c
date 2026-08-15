#include "app_state.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static app_state_t s_state;
static SemaphoreHandle_t s_mtx;

void app_state_init(void)
{
    s_mtx = xSemaphoreCreateRecursiveMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_state.net = NET_BOOT;
}

void app_state_lock(void)   { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void app_state_unlock(void) { xSemaphoreGiveRecursive(s_mtx); }
app_state_t *app_state_get(void) { return &s_state; }
void app_state_touch(void)  { s_state.seq++; }

void app_state_snapshot(app_state_t *out)
{
    app_state_lock();
    memcpy(out, &s_state, sizeof(*out));
    app_state_unlock();
}
