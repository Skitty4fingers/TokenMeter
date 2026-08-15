#include "board/lcd.h"
#include "board/pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl_private.h"   /* disp->flushing */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CH      LEDC_CHANNEL_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES     LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ    5000

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_LEDC_MODE, .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER, .freq_hz = BL_LEDC_FREQ, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL, .speed_mode = BL_LEDC_MODE, .channel = BL_LEDC_CH,
        .timer_sel = BL_LEDC_TIMER, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void lcd_set_brightness(uint8_t pct)
{
    if (pct > 50) pct = 50;
    uint32_t duty = (1023u * pct) / 100u;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CH, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CH);
}

lv_display_t *lcd_init(uint8_t rotation)
{
    backlight_init();
    lcd_set_brightness(0);

    ESP_LOGI(TAG, "SPI bus");
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,     /* SD shares this bus */
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT));
    /* landscape: after swap_xy the RASET axis is the physical 172-px column axis */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, LCD_X_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 6144;
    port_cfg.task_affinity = -1;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 24,      /* ~15 KB per buffer, 2 buffers */
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = (rotation == 0),
            .mirror_y = (rotation != 0),
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,   /* SPI panel is big-endian RGB565 */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    return s_disp;
}

bool lcd_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void lcd_unlock(void)              { lvgl_port_unlock(); }

bool lcd_bus_quiesce(uint32_t timeout_ms)
{
    if (!lvgl_port_lock(timeout_ms)) return false;
    /* LVGL returns from a refresh with the last flush still in flight; wait for the ISR to clear it */
    for (int i = 0; i < 100 && s_disp && s_disp->flushing; i++) vTaskDelay(pdMS_TO_TICKS(2));
    if (s_disp && s_disp->flushing) { lvgl_port_unlock(); return false; }
    /* trans-done fires when DMA is drained, the SPI FIFO can still be shifting for a few µs;
       a polling sdspi transaction started in that window trips the driver assert. Settle. */
    vTaskDelay(pdMS_TO_TICKS(3));
    return true;
}
void lcd_bus_release(void) { lvgl_port_unlock(); }
