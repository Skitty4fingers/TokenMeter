#pragma once
/* Waveshare ESP32-C6-LCD-1.47 pin map (from the Waveshare wiki) */

#define PIN_LCD_MOSI   6
#define PIN_LCD_SCLK   7
#define PIN_LCD_CS     14
#define PIN_LCD_DC     15
#define PIN_LCD_RST    21
#define PIN_LCD_BL     22

#define PIN_SD_MISO    5
#define PIN_SD_CS      4    /* SD shares MOSI/SCLK with the LCD */

#define PIN_RGB_LED    8    /* single WS2812 */

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PCLK_HZ    (40 * 1000 * 1000)

/* Panel is 172(w) x 320(h) native (portrait); we run it landscape */
#define LCD_NATIVE_W   172
#define LCD_NATIVE_H   320
#define LCD_H_RES      320  /* logical, after rotation */
#define LCD_V_RES      172
#define LCD_X_GAP      34   /* 240-172 = 68 → 34 each side on the ST7789 frame */

/* Set to 1 if red and blue come out swapped on your unit. */
#define LCD_BGR_ORDER  0
/* ST7789V3 IPS panels on this board need color inversion. */
#define LCD_INVERT     1
