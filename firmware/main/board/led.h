#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { LED_OFF, LED_GREEN, LED_AMBER, LED_RED, LED_BLUE_PULSE, LED_AMBER_BLINK } led_mode_t;
void led_init(void);
void led_set(led_mode_t m);
void led_set_enabled(bool en);
#ifdef __cplusplus
}
#endif
