#include "board/sdcard.h"
#include "board/pins.h"
#include <stdio.h>
#include <string.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board/lcd.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static bool s_mounted;
static size_t s_total_kb, s_used_kb;   /* measured once at mount; used_kb bumped per append */
#define MOUNT "/sdcard"
#define CSV   MOUNT "/usage.csv"

bool sdcard_mount(void)
{
    if (!lcd_bus_quiesce(1000)) { ESP_LOGW(TAG, "bus busy, skipping mount"); return false; }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_SPI_HOST;
    host.max_freq_khz = 10000;    /* keep SD slow-ish; it shares the bus with the LCD */
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t m = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &m, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card (%s)", esp_err_to_name(err));
        s_mounted = false;
        lcd_bus_release();
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    /* header once */
    FILE *f = fopen(CSV, "r");
    if (!f) {
        f = fopen(CSV, "w");
        if (f) { fputs("ts,provider,window,pct,resets_at\n", f); }
    }
    if (f) fclose(f);
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &free_b) == ESP_OK) { s_total_kb = total >> 10; s_used_kb = (total - free_b) >> 10; }
    lcd_bus_release();
    return true;
}

bool sdcard_mounted(void) { return s_mounted; }

void sdcard_stats(size_t *total_kb, size_t *used_kb)
{
    /* No SD I/O here on purpose: /api/status is polled constantly and every card read races the
       LCD DMA on the shared bus. Numbers come from mount time + bytes we appended since. */
    *total_kb = s_mounted ? s_total_kb : 0;
    *used_kb  = s_mounted ? s_used_kb  : 0;
}

bool sdcard_append_csv(const char *line)
{
    if (!s_mounted) return false;
    if (!lcd_bus_quiesce(500)) return false;
    FILE *f = fopen(CSV, "a");
    bool ok = false;
    if (f) { fputs(line, f); fputc('\n', f); fclose(f); ok = true; }
    lcd_bus_release();
    static size_t acc;
    if (ok) { acc += strlen(line) + 1; if (acc >= 1024) { s_used_kb += acc >> 10; acc &= 1023; } }
    return ok;
}
