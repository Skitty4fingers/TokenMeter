#include "ui/ui_provision.h"
#include "ui/theme.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr, *s_qr, *s_ssid, *s_pass, *s_eyebrow, *s_title;

lv_obj_t *ui_provision_create(void)
{
    s_scr = lv_obj_create(NULL);
    theme_plain(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* QR: 108 px, white quiet zone */
    s_qr = lv_qrcode_create(s_scr);
    lv_qrcode_set_size(s_qr, 100);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_color(s_qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_qr, 4, 0);
    lv_obj_set_style_radius(s_qr, 3, 0);
    lv_obj_set_pos(s_qr, 10, 32);

    int x = 130;
    lv_obj_t *l;
    s_eyebrow = theme_label(s_scr, FONT_LABEL, COL_WARN, "SETUP MODE");
    lv_obj_set_style_text_letter_space(s_eyebrow, 2, 0);
    lv_obj_set_pos(s_eyebrow, x, 18);
    s_title = theme_label(s_scr, FONT_BODY, COL_INK, "Join this hotspot");
    lv_obj_set_pos(s_title, x, 33);
    l = theme_label(s_scr, FONT_LABEL, COL_MUTED, "network");
    lv_obj_set_pos(l, x, 56);
    s_ssid = theme_label(s_scr, FONT_PCT, COL_INK, "TokenMeter-????");
    lv_obj_set_pos(s_ssid, x, 70);
    l = theme_label(s_scr, FONT_LABEL, COL_MUTED, "password");
    lv_obj_set_pos(l, x, 94);
    s_pass = theme_label(s_scr, FONT_PCT, COL_INK, "");
    lv_obj_set_style_text_letter_space(s_pass, 1, 0);
    lv_obj_set_pos(s_pass, x, 108);
    l = theme_label(s_scr, FONT_HEAD, COL_MUTED, "then open  ");
    lv_obj_set_pos(l, x, 138);
    l = theme_label(s_scr, FONT_HEAD, COL_INK, "192.168.4.1");
    lv_obj_set_pos(l, x + 78, 138);
    return s_scr;
}

void ui_provision_set(const char *ssid, const char *pass, bool offline)
{
    lv_label_set_text(s_eyebrow, offline ? "WIFI UNREACHABLE" : "SETUP MODE");
    lv_label_set_text(s_title, offline ? "Fix WiFi via hotspot" : "Join this hotspot");
    lv_label_set_text(s_ssid, ssid);
    lv_label_set_text(s_pass, pass);
    char wifi[128];
    snprintf(wifi, sizeof(wifi), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    lv_qrcode_update(s_qr, wifi, strlen(wifi));
}
