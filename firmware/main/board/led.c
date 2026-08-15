#include "board/led.h"
#include "board/pins.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"

static led_strip_handle_t s_strip;
static led_mode_t s_mode = LED_OFF;
static bool s_enabled = true;
static esp_timer_handle_t s_tmr;
static int s_phase;

#define BRI 0.20f  /* keep it a status light, not a lamp */

static void show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r * BRI, g * BRI, b * BRI);
    led_strip_refresh(s_strip);
}

static void tick(void *arg)
{
    s_phase = (s_phase + 1) % 20;   /* 100 ms ticks → 2 s cycle */
    if (!s_enabled) { show(0, 0, 0); return; }
    switch (s_mode) {
    case LED_OFF:   show(0, 0, 0); break;
    case LED_GREEN: show(0x19, 0xA3, 0x7F); break;
    case LED_AMBER: show(0xF0, 0xB4, 0x29); break;
    case LED_RED:   show(0xF0, 0x4E, 0x4E); break;
    case LED_BLUE_PULSE: {   /* soft triangle wave */
        int p = s_phase < 10 ? s_phase : 19 - s_phase;
        show(0, 0x40 * p / 10, 0xFF * p / 10);
        break;
    }
    case LED_AMBER_BLINK:
        if (s_phase < 4) show(0xF0, 0xB4, 0x29); else show(0, 0, 0);
        break;
    }
}

void led_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW("led", "no LED strip");
        return;
    }
    led_strip_clear(s_strip);
    const esp_timer_create_args_t a = { .callback = tick, .name = "led" };
    esp_timer_create(&a, &s_tmr);
    esp_timer_start_periodic(s_tmr, 100 * 1000);
}

void led_set(led_mode_t m)      { s_mode = m; }
void led_set_enabled(bool en)   { s_enabled = en; }
