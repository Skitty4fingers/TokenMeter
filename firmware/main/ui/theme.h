#pragma once
#include "lvgl.h"
/* Palette from design/ui-samples.html §6 */
#define COL_BG        0x0A0C10
#define COL_INK       0xECEFF3
#define COL_MUTED     0x7C8794
#define COL_TRACK     0x1F2530
#define COL_RULE      0x171C24
#define COL_CLAUDE    0xD97757
#define COL_GPT       0x19A37F
#define COL_WARN      0xF0B429
#define COL_CRIT      0xF04E4E
#define COL_OFFLINE   0x4A525C
#define COL_BANNER_BG 0x2A2208
#define COL_BANNER_LN 0x5B4A0E

#define FONT_LABEL   (&lv_font_montserrat_12)
#define FONT_HEAD    (&lv_font_montserrat_14)
#define FONT_BODY    (&lv_font_montserrat_14)
#define FONT_PCT     (&lv_font_montserrat_18)
#define FONT_PCT_BIG  (&lv_font_montserrat_24)
#define FONT_PCT_HERO (&lv_font_montserrat_48)

#define BULLET "\xE2\x80\xA2"   /* • */

/* shared style helpers */
void theme_init(void);
lv_obj_t *theme_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt);
void theme_plain(lv_obj_t *o);   /* no bg, no border, no padding, no scroll */
