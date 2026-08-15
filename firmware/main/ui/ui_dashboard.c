/* Dashboard + boot + offline banner + provider error rows.
 * Geometry mirrors design/ui-samples.html so the LCD matches the mockups 1:1. */
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ui_provision.h"
#include "app_state.h"
#include "storage/config.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 320
#define H 172
#define PAD 6

/* ---- geometry per layout ---- */
typedef struct { int hdr_h, row_h, win_w, gap, pct_w, reset_w, bar_h; const lv_font_t *pct_font, *head_font; } geo_t;
static const geo_t GEO_DUAL = { 18, 24, 26, 8, 46, 76, 8,  FONT_PCT,     FONT_HEAD };
static const geo_t GEO_SOLO = { 22, 30, 46, 10, 56, 80, 10, FONT_PCT_BIG, FONT_HEAD };

/* ---- widgets ---- */
typedef struct {
    lv_obj_t *cont, *win, *bar, *pct, *reset, *msg;
    time_t resets_at;
} row_w_t;

typedef struct {
    lv_obj_t *cont, *name, *plan;
    row_w_t rows[PROV_MAX_WIN];
    int nrows;
    provider_id_t id;
} prov_w_t;

static lv_obj_t *s_scr_dash, *s_scr_boot, *s_scr_prov;
static lv_obj_t *s_status, *s_st_left, *s_st_right, *s_banner, *s_bn_left, *s_bn_right;
static lv_obj_t *s_boot_msg, *s_boot_bar, *s_boot_ver;
static prov_w_t s_prov[PROV_COUNT];
static int s_nprov;
static uint32_t s_sig;            /* structural signature of what's built */
static uint32_t s_seq_seen;
static bool s_force;

/* ---- helpers ---- */
static uint32_t color_for(provider_id_t id, float pct, const app_config_t *cfg, bool offline)
{
    if (offline) return COL_OFFLINE;
    if (pct >= cfg->crit_pct) return COL_CRIT;
    if (pct >= cfg->warn_pct) return COL_WARN;
    return id == PROV_CLAUDE ? COL_CLAUDE : COL_GPT;
}

static void fmt_countdown(time_t resets, char *out, size_t n)
{
    time_t now = time(NULL);
    if (!resets || now < 1700000000) { snprintf(out, n, LV_SYMBOL_REFRESH " --"); return; }
    long d = (long)(resets - now);
    if (d <= 0) { snprintf(out, n, LV_SYMBOL_REFRESH " now"); return; }
    if (d >= 86400) snprintf(out, n, LV_SYMBOL_REFRESH " %ldd %ldh", d / 86400, (d % 86400) / 3600);
    else if (d >= 3600) snprintf(out, n, LV_SYMBOL_REFRESH " %ldh %02ldm", d / 3600, (d % 3600) / 60);
    else snprintf(out, n, LV_SYMBOL_REFRESH " %ldm", d / 60);
}

static void fmt_ago(time_t t, char *out, size_t n)
{
    time_t now = time(NULL);
    if (!t) { snprintf(out, n, "no data"); return; }
    long d = (long)(now - t);
    if (d < 60) snprintf(out, n, "%lds ago", d);
    else if (d < 3600) snprintf(out, n, "%ldm ago", d / 60);
    else snprintf(out, n, "%ldh ago", d / 3600);
}

static void fmt_clock(char *out, size_t n, bool synced)
{
    if (!synced) { strlcpy(out, "--:--", n); return; }
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(out, n, "%H:%M", &tm);
}

/* ---- construction ---- */
static void build_bar(lv_obj_t *bar, int h, uint32_t col)
{
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_height(bar, h);
}

static void build_provider(prov_w_t *p, provider_id_t id, int nrows, const geo_t *g, int y)
{
    p->id = id; p->nrows = nrows;
    p->cont = lv_obj_create(s_scr_dash);
    theme_plain(p->cont);
    lv_obj_set_pos(p->cont, PAD, y);
    lv_obj_set_size(p->cont, W - 2 * PAD, g->hdr_h + nrows * g->row_h);

    p->name = theme_label(p->cont, g->head_font, id == PROV_CLAUDE ? COL_CLAUDE : COL_GPT, id == PROV_CLAUDE ? "CLAUDE" : "CHATGPT");
    lv_obj_set_style_text_letter_space(p->name, 1, 0);
    lv_obj_align(p->name, LV_ALIGN_TOP_LEFT, 2, (g->hdr_h - 14) / 2 - 1);
    p->plan = theme_label(p->cont, FONT_LABEL, COL_MUTED, "");
    lv_obj_align(p->plan, LV_ALIGN_TOP_RIGHT, -2, (g->hdr_h - 12) / 2);

    int cw = W - 2 * PAD - 4;                                   /* content width inside 2px pads */
    int track_w = cw - g->win_w - g->pct_w - g->reset_w - 3 * g->gap;
    for (int i = 0; i < nrows; i++) {
        row_w_t *r = &p->rows[i];
        r->cont = lv_obj_create(p->cont);
        theme_plain(r->cont);
        lv_obj_set_pos(r->cont, 2, g->hdr_h + i * g->row_h);
        lv_obj_set_size(r->cont, cw, g->row_h);

        r->win = theme_label(r->cont, FONT_LABEL, COL_MUTED, "");
        lv_obj_set_width(r->win, g->win_w);
        lv_obj_align(r->win, LV_ALIGN_LEFT_MID, 0, 0);

        r->bar = lv_bar_create(r->cont);
        build_bar(r->bar, g->bar_h, COL_CLAUDE);
        lv_obj_set_width(r->bar, track_w);
        lv_obj_align(r->bar, LV_ALIGN_LEFT_MID, g->win_w + g->gap, 0);

        r->pct = theme_label(r->cont, g->pct_font, COL_INK, "");
        lv_obj_set_width(r->pct, g->pct_w);
        lv_obj_set_style_text_align(r->pct, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r->pct, LV_ALIGN_LEFT_MID, g->win_w + g->gap + track_w + g->gap, 0);

        r->reset = theme_label(r->cont, FONT_LABEL, COL_MUTED, "");
        lv_obj_set_width(r->reset, g->reset_w);
        lv_obj_align(r->reset, LV_ALIGN_LEFT_MID, g->win_w + 3 * g->gap + track_w + g->pct_w, 0);

        /* error message label, hidden unless the row is in error */
        r->msg = theme_label(r->cont, FONT_LABEL, COL_CRIT, "");
        lv_obj_set_width(r->msg, cw - g->win_w - g->gap);
        lv_label_set_long_mode(r->msg, LV_LABEL_LONG_DOT);
        lv_obj_align(r->msg, LV_ALIGN_LEFT_MID, g->win_w + g->gap, 0);
        lv_obj_add_flag(r->msg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Single-window solo provider (e.g. ChatGPT with only a weekly window): a big centered gauge
   that fills the panel instead of one thin bar. Reuses row_w_t[0] so update_values() drives it. */
static void build_hero(prov_w_t *p, provider_id_t id, int avail_h)
{
    p->id = id; p->nrows = 1;
    p->cont = lv_obj_create(s_scr_dash);
    theme_plain(p->cont);
    lv_obj_set_pos(p->cont, PAD, PAD);
    lv_obj_set_size(p->cont, W - 2 * PAD, avail_h);
    int cw = W - 2 * PAD - 4;

    p->name = theme_label(p->cont, FONT_HEAD, id == PROV_CLAUDE ? COL_CLAUDE : COL_GPT, id == PROV_CLAUDE ? "CLAUDE" : "CHATGPT");
    lv_obj_set_style_text_letter_space(p->name, 1, 0);
    lv_obj_align(p->name, LV_ALIGN_TOP_LEFT, 2, 2);
    p->plan = theme_label(p->cont, FONT_LABEL, COL_MUTED, "");
    lv_obj_align(p->plan, LV_ALIGN_TOP_RIGHT, -2, 4);

    row_w_t *r = &p->rows[0];
    r->cont = p->cont;   /* hero uses the provider container directly */

    /* big percentage, centered, upper-middle */
    r->pct = theme_label(p->cont, FONT_PCT_HERO, COL_INK, "");
    lv_obj_align(r->pct, LV_ALIGN_CENTER, 0, -18);

    /* full-width thick bar under the number */
    r->bar = lv_bar_create(p->cont);
    build_bar(r->bar, 22, id == PROV_CLAUDE ? COL_CLAUDE : COL_GPT);
    lv_obj_set_width(r->bar, cw);
    lv_obj_align(r->bar, LV_ALIGN_CENTER, 0, 30);

    /* footer: window label (left) + reset countdown (right), just under the bar */
    r->win = theme_label(p->cont, FONT_LABEL, COL_MUTED, "");
    lv_obj_align(r->win, LV_ALIGN_CENTER, -(cw / 2) + 6, 54);
    r->reset = theme_label(p->cont, FONT_LABEL, COL_MUTED, "");
    lv_obj_set_style_text_align(r->reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(r->reset, LV_ALIGN_CENTER, (cw / 2) - 6 - 80, 54);
    lv_obj_set_width(r->reset, 80);

    /* error message (centered) reuses msg */
    r->msg = theme_label(p->cont, FONT_HEAD, COL_CRIT, "");
    lv_label_set_long_mode(r->msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(r->msg, cw - 8);
    lv_obj_set_style_text_align(r->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(r->msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(r->msg, LV_OBJ_FLAG_HIDDEN);
}

static void clear_providers(void)
{
    for (int i = 0; i < s_nprov; i++) if (s_prov[i].cont) { lv_obj_delete(s_prov[i].cont); s_prov[i].cont = NULL; }
    s_nprov = 0;
}

/* signature: which providers enabled + how many rows each */
static uint32_t structure_sig(const app_state_t *st)
{
    uint32_t s = 0;
    for (int i = 0; i < PROV_COUNT; i++) {
        const provider_usage_t *p = &st->prov[i];
        int rows = p->enabled ? (p->nwin ? p->nwin : 2) : 0;
        s = s * 31 + (p->enabled ? 1 : 0) * 8 + rows;
    }
    return s;
}

static void rebuild(const app_state_t *st)
{
    clear_providers();
    int enabled = 0;
    for (int i = 0; i < PROV_COUNT; i++) if (st->prov[i].enabled) enabled++;
    const geo_t *g = enabled == 1 ? &GEO_SOLO : &GEO_DUAL;
    int y = PAD;
    if (enabled == 1) {
        int avail = H - 20 - 2 * PAD;                          /* area above the status strip */
        for (int i = 0; i < PROV_COUNT; i++) {
            const provider_usage_t *p = &st->prov[i];
            if (!p->enabled) continue;
            int rows = p->nwin ? p->nwin : 2;
            if (rows > 4) rows = 4;
            if (rows == 1) {
                build_hero(&s_prov[s_nprov++], (provider_id_t)i, avail);   /* one window: big centered gauge */
            } else {
                /* multiple windows: taller rows so the block fills the screen */
                geo_t gg = GEO_SOLO;
                gg.row_h = (avail - gg.hdr_h) / rows;
                if (gg.row_h > 40) gg.row_h = 40;
                gg.bar_h = gg.row_h / 3; if (gg.bar_h > 14) gg.bar_h = 14; if (gg.bar_h < 8) gg.bar_h = 8;
                int block = gg.hdr_h + rows * gg.row_h;
                y = PAD + (avail - block) / 2; if (y < PAD) y = PAD;
                build_provider(&s_prov[s_nprov++], (provider_id_t)i, rows, &gg, y);
            }
        }
    } else {
        for (int i = 0; i < PROV_COUNT; i++) {
            const provider_usage_t *p = &st->prov[i];
            if (!p->enabled) continue;
            int rows = p->nwin ? p->nwin : 2;
            if (rows > 2) rows = 2;                             /* dual layout only has room for 5h + wk */
            build_provider(&s_prov[s_nprov++], (provider_id_t)i, rows, g, y);
            y += g->hdr_h + rows * g->row_h + PAD;
        }
    }
    /* keep status/banner on top */
    lv_obj_move_foreground(s_status);
    lv_obj_move_foreground(s_banner);
    s_sig = structure_sig(st);
}

/* ---- per-tick value refresh ---- */
static void update_values(const app_state_t *st, const app_config_t *cfg)
{
    bool offline = st->net != NET_ONLINE;
    for (int k = 0; k < s_nprov; k++) {
        prov_w_t *pw = &s_prov[k];
        const provider_usage_t *p = &st->prov[pw->id];
        lv_label_set_text(pw->plan, p->plan[0] ? p->plan : "");
        lv_obj_set_style_text_color(pw->name, lv_color_hex(offline ? COL_OFFLINE : (pw->id == PROV_CLAUDE ? COL_CLAUDE : COL_GPT)), 0);
        for (int i = 0; i < pw->nrows; i++) {
            row_w_t *r = &pw->rows[i];
            const usage_window_t *w = i < p->nwin ? &p->win[i] : NULL;
            bool err_row = (!p->ok && p->err[0]) && (i == 0 || (i == 1 && !w));   /* show error on row 0 (and 1 if empty) */
            const char *labels_default[2] = { "5h", "wk" };
            lv_label_set_text(r->win, w && w->label[0] ? w->label : (i < 2 ? labels_default[i] : ""));
            if (err_row && !(w && p->fetched_at)) {
                /* never had data → error message replaces the bar */
                lv_obj_add_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(r->pct, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(r->reset, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(r->msg, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(r->msg, i == 0 ? p->err : "");
                continue;
            }
            lv_obj_remove_flag(r->bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(r->pct, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(r->reset, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->msg, LV_OBJ_FLAG_HIDDEN);
            if (!w) { lv_bar_set_value(r->bar, 0, LV_ANIM_OFF); lv_label_set_text(r->pct, "--"); lv_label_set_text(r->reset, ""); continue; }
            int pct = (int)(w->pct + 0.5f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            uint32_t col = color_for(pw->id, w->pct, cfg, offline);
            lv_obj_set_style_bg_color(r->bar, lv_color_hex(col), LV_PART_INDICATOR);
            lv_bar_set_value(r->bar, pct, LV_ANIM_ON);
            char b[16]; snprintf(b, sizeof(b), "%d%%", pct);
            lv_label_set_text(r->pct, b);
            lv_obj_set_style_text_color(r->pct, lv_color_hex(offline ? COL_OFFLINE : (col == COL_WARN || col == COL_CRIT ? col : COL_INK)), 0);
            r->resets_at = w->resets_at;
            char c[24]; fmt_countdown(w->resets_at, c, sizeof(c));
            lv_label_set_text(r->reset, c);
            /* stale (fetch failing but we have old data): dim the row */
            lv_obj_set_style_opa(r->cont, (!p->ok && p->fetched_at) ? LV_OPA_60 : LV_OPA_COVER, 0);
        }
    }
}

static void update_status(const app_state_t *st)
{
    char l[64], r[64], ago[16], clk[8];
    bool online = st->net == NET_ONLINE;
    if (online) {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        if (st->ip[0]) snprintf(l, sizeof(l), LV_SYMBOL_WIFI " %.14s " BULLET " %s", st->ssid, st->ip);
        else           snprintf(l, sizeof(l), LV_SYMBOL_WIFI " %s", st->ssid);
        time_t last = 0; int errs = 0;
        for (int i = 0; i < PROV_COUNT; i++) {
            if (!st->prov[i].enabled) continue;
            if (st->prov[i].fetched_at > last) last = st->prov[i].fetched_at;
            if (!st->prov[i].ok && st->prov[i].err[0]) errs++;
        }
        fmt_ago(last, ago, sizeof(ago));
        fmt_clock(clk, sizeof(clk), st->time_synced);
        if (errs) snprintf(r, sizeof(r), "#F04E4E " LV_SYMBOL_WARNING " %d error#  %s", errs, clk);
        else snprintf(r, sizeof(r), LV_SYMBOL_OK " %s  %s", ago, clk);
        lv_label_set_text(s_st_left, l);
        lv_label_set_text(s_st_right, r);
    } else {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        time_t last = 0;
        for (int i = 0; i < PROV_COUNT; i++) if (st->prov[i].fetched_at > last) last = st->prov[i].fetched_at;
        char lastc[8] = "--:--";
        if (last) { struct tm tm; localtime_r(&last, &tm); strftime(lastc, sizeof(lastc), "%H:%M", &tm); }
        snprintf(l, sizeof(l), "OFFLINE " BULLET " %s " BULLET " last %s", st->net == NET_CONNECTING ? "connecting" : "retrying", lastc);
        snprintf(r, sizeof(r), "AP %s", st->ap_ssid);
        lv_label_set_text(s_bn_left, l);
        lv_label_set_text(s_bn_right, r);
    }
}

/* ---- screens ---- */
static void build_dashboard(void)
{
    s_scr_dash = lv_obj_create(NULL);
    theme_plain(s_scr_dash);
    lv_obj_set_style_bg_color(s_scr_dash, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr_dash, LV_OPA_COVER, 0);

    /* status strip */
    s_status = lv_obj_create(s_scr_dash);
    theme_plain(s_status);
    lv_obj_set_pos(s_status, PAD, H - 20);
    lv_obj_set_size(s_status, W - 2 * PAD, 20);
    lv_obj_set_style_border_color(s_status, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_border_width(s_status, 1, 0);
    lv_obj_set_style_border_side(s_status, LV_BORDER_SIDE_TOP, 0);
    s_st_left = theme_label(s_status, FONT_LABEL, COL_MUTED, "");
    lv_obj_align(s_st_left, LV_ALIGN_LEFT_MID, 2, 1);
    s_st_right = theme_label(s_status, FONT_LABEL, COL_MUTED, "");
    lv_label_set_recolor(s_st_right, true);
    lv_obj_align(s_st_right, LV_ALIGN_RIGHT_MID, -2, 1);

    /* offline banner */
    s_banner = lv_obj_create(s_scr_dash);
    theme_plain(s_banner);
    lv_obj_set_pos(s_banner, 0, H - 22);
    lv_obj_set_size(s_banner, W, 22);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(COL_BANNER_BG), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(COL_BANNER_LN), 0);
    lv_obj_set_style_border_width(s_banner, 1, 0);
    lv_obj_set_style_border_side(s_banner, LV_BORDER_SIDE_TOP, 0);
    s_bn_left = theme_label(s_banner, FONT_LABEL, COL_WARN, "");
    lv_obj_align(s_bn_left, LV_ALIGN_LEFT_MID, PAD, 0);
    s_bn_right = theme_label(s_banner, FONT_LABEL, COL_WARN, "");
    lv_obj_align(s_bn_right, LV_ALIGN_RIGHT_MID, -PAD, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

static void boot_anim_cb(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }

static void build_boot(void)
{
    s_scr_boot = lv_obj_create(NULL);
    theme_plain(s_scr_boot);
    lv_obj_set_style_bg_color(s_scr_boot, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr_boot, LV_OPA_COVER, 0);

    lv_obj_t *logo = theme_label(s_scr_boot, FONT_BODY, COL_INK, "TOKEN #D97757 METER#");
    lv_label_set_recolor(logo, true);
    lv_obj_set_style_text_letter_space(logo, 3, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -22);

    s_boot_msg = theme_label(s_scr_boot, FONT_HEAD, COL_MUTED, "starting");
    lv_obj_align(s_boot_msg, LV_ALIGN_CENTER, 0, 2);

    lv_obj_t *track = lv_obj_create(s_scr_boot);
    theme_plain(track);
    lv_obj_set_size(track, 120, 3);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 22);
    s_boot_bar = lv_obj_create(track);
    theme_plain(s_boot_bar);
    lv_obj_set_size(s_boot_bar, 48, 3);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_bg_opa(s_boot_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_boot_bar, 2, 0);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, s_boot_bar);
    lv_anim_set_values(&a, 0, 72);
    lv_anim_set_duration(&a, 1400);
    lv_anim_set_playback_duration(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, boot_anim_cb);
    lv_anim_start(&a);

    char v[48]; snprintf(v, sizeof(v), "v%.16s " BULLET " c6", esp_app_get_description()->version);
    s_boot_ver = theme_label(s_scr_boot, FONT_LABEL, COL_OFFLINE, v);
    lv_obj_align(s_boot_ver, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
}

/* ---- tick ---- */
static void tick(lv_timer_t *t)
{
    static app_state_t st;           /* static: too big for the LVGL task stack */
    app_state_snapshot(&st);
    const app_config_t *cfg = config_get();
    bool changed = st.seq != s_seq_seen || s_force;
    s_seq_seen = st.seq; s_force = false;

    int enabled = 0;
    for (int i = 0; i < PROV_COUNT; i++) if (st.prov[i].enabled) enabled++;

    lv_obj_t *want;
    switch (st.net) {
    case NET_PROVISION:
        want = s_scr_prov;
        if (changed) ui_provision_set(st.ap_ssid, st.ap_pass, false);
        break;
    case NET_BOOT:
    case NET_CONNECTING: {
        bool have_data = false;
        for (int i = 0; i < PROV_COUNT; i++) if (st.prov[i].fetched_at) have_data = true;
        if (have_data && st.net == NET_CONNECTING) { want = s_scr_dash; break; }
        want = s_scr_boot;
        char m[64]; snprintf(m, sizeof(m), "connecting to %s...", st.ssid);
        lv_label_set_text(s_boot_msg, m);
        break;
    }
    case NET_ONLINE:
        if (enabled == 0) {
            want = s_scr_boot;
            char m[64]; snprintf(m, sizeof(m), "add a provider at http://%s", st.ip);
            lv_label_set_text(s_boot_msg, m);
            break;
        }
        want = s_scr_dash; break;
    case NET_FALLBACK: {
        /* nothing cached to grey out (e.g. outage at boot) → show the hotspot card instead of an empty dashboard */
        bool have_data = false;
        for (int i = 0; i < PROV_COUNT; i++) if (st.prov[i].fetched_at) have_data = true;
        if (!have_data) { want = s_scr_prov; if (changed) ui_provision_set(st.ap_ssid, st.ap_pass, true); break; }
        want = s_scr_dash; break;
    }
    default:
        want = s_scr_dash; break;
    }
    if (lv_screen_active() != want) lv_screen_load(want);
    if (want != s_scr_dash) return;

    if (structure_sig(&st) != s_sig) { rebuild(&st); changed = true; }
    if (changed) update_values(&st, cfg);
    else {
        /* countdowns tick every second even without new data */
        for (int k = 0; k < s_nprov; k++) for (int i = 0; i < s_prov[k].nrows; i++) {
            row_w_t *r = &s_prov[k].rows[i];
            if (r->resets_at && !lv_obj_has_flag(r->reset, LV_OBJ_FLAG_HIDDEN)) {
                char c[24]; fmt_countdown(r->resets_at, c, sizeof(c)); lv_label_set_text(r->reset, c);
            }
        }
    }
    update_status(&st);
}

void ui_request_refresh(void) { s_force = true; }

void ui_init(void)
{
    theme_init();
    build_boot();
    build_dashboard();
    s_scr_prov = ui_provision_create();
    lv_screen_load(s_scr_boot);
    lv_timer_create(tick, 1000, NULL);
    s_force = true;
}
