#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init SPI bus + ST7789 + LVGL port. rotation: 0 = USB on the left, 1 = USB on the right. */
lv_display_t *lcd_init(uint8_t rotation);
/* 0..100, silently capped at 50 (Waveshare thermal guidance) */
void lcd_set_brightness(uint8_t pct);
/* take/release the LVGL lock — required around any lv_* call from another task */
bool lcd_lock(uint32_t timeout_ms);
void lcd_unlock(void);
/* The SD card shares SPI2 with the panel and the IDF sdspi/esp_lcd drivers race on the C6
   (assert in spi_hal_setup_trans). Bracket ALL SD access with these: they take the LVGL lock
   (no new flushes) and wait for the in-flight DMA flush to finish. */
bool lcd_bus_quiesce(uint32_t timeout_ms);
void lcd_bus_release(void);

#ifdef __cplusplus
}
#endif
