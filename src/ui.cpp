// M3 UI: tileview (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
#include "ui.h"
#include "radar_view.h"
#include "route.h"
#include "photo.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include "splash_jpg.h"
#endif

#define UI_GREEN lv_color_hex(0x1DFF86)
#define UI_INK   lv_color_hex(0xEAFFF3)
#define UI_SOFT  lv_color_hex(0x9AFFC8)
#define UI_DIM   lv_color_hex(0x5F7A6C)
#define UI_PANEL lv_color_hex(0x0C160F)
#define UI_EMERG lv_color_hex(0xFF5A3C)

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr;
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardL = nullptr, *s_cardR = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static lv_obj_t *s_photo = nullptr, *s_photoCredit = nullptr;   // aircraft photo above the card
static char s_lastRouteReq[12] = "";
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_hudBars[4] = { nullptr, nullptr, nullptr, nullptr };   // WiFi signal-strength bars
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_statsLbl = nullptr;
static lv_obj_t *s_statsNet = nullptr;
static lv_obj_t *s_hudGps   = nullptr;   // HUD satellite icon (hidden unless GPS auto-location is on)
static lv_obj_t *s_statsGps = nullptr;   // Stats view GPS status line

// --------------------------------------------------------------------- units
// 0 = Aviation (ft, kt, km) · 1 = Metric (m, km/h, km) · 2 = Imperial (ft, mph, mi).
// The feed gives altitude in ft, speed in kt, vertical speed in fpm, distance in km.
static int s_units = 0;
void ui_set_units(int u) { s_units = (u < 0 || u > 2) ? 0 : u; }

static void fmt_alt(char *b, size_t n, float ft, bool gnd) {
    if (gnd)            snprintf(b, n, "GND");
    else if (s_units == 1) snprintf(b, n, "%.0f m",  ft * 0.3048f);
    else                snprintf(b, n, "%.0f ft", ft);
}
static void fmt_spd(char *b, size_t n, float kt) {
    if (kt != kt)          snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%.0f km/h", kt * 1.852f);
    else if (s_units == 2) snprintf(b, n, "%.0f mph",  kt * 1.15078f);
    else                   snprintf(b, n, "%.0f kt",   kt);
}
static void fmt_vs(char *b, size_t n, float fpm) {
    if (fpm != fpm)        snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%+.1f m/s", fpm * 0.00508f);
    else                   snprintf(b, n, "%+.0f fpm", fpm);
}
static float dist_val(float km) {
    if (s_units == 0) return km * 0.539957f;   // Aviation -> nautical miles
    if (s_units == 2) return km * 0.621371f;   // Imperial -> miles
    return km;                                   // Metric   -> km
}
static const char *dist_unit(void) { return s_units == 0 ? "nm" : (s_units == 2 ? "mi" : "km"); }

// Fold Latin-1 accents / drop any other non-ASCII so the Montserrat font never hits a
// missing glyph (which renders as an empty box). Belt-and-suspenders for card text.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

// ----------------------------------------------------------------- detail card
static void refresh_card(void) {
    AcInfo in;
    if (!radar::selected(in)) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (s_photo)       lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        s_lastRouteReq[0] = 0;
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    char title[40];
    if (in.type[0]) snprintf(title, sizeof(title), "%s  %s", in.call[0] ? in.call : "-", in.type);
    else            snprintf(title, sizeof(title), "%s", in.call[0] ? in.call : "-");
    fold_ascii(title);
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.emergency ? UI_EMERG : UI_INK, 0);

    char altS[16], vsS[24], spdS[16], sqS[16];
    fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_vs (vsS,  sizeof(vsS),  in.vsFpm);
    fmt_spd(spdS, sizeof(spdS), in.gsKt);
    if (in.squawk < 0)          snprintf(sqS, sizeof(sqS), "-");
    else                        snprintf(sqS, sizeof(sqS), "%04d", in.squawk);

    char left[96], right[96];
    snprintf(left,  sizeof(left),  "ALT  %s\nSPD  %s\nDIST %.1f %s", altS, spdS, dist_val(in.distKm), dist_unit());
    snprintf(right, sizeof(right), "V/S  %s\nHDG  %03.0f\nSQK  %s", vsS, in.bearingDeg, sqS);
    lv_label_set_text(s_cardL, left);
    lv_label_set_text(s_cardR, right);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call);
    }
    char rfrom[40], rto[40];
    if (!in.call[0]) {
        lv_label_set_text(s_cardRoute, "Route -");                 // no callsign -> nothing to look up
    } else if (route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) snprintf(rt, sizeof(rt), "%s -> %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        else                    snprintf(rt, sizeof(rt), "Route unavailable");
        fold_ascii(rt);
        lv_label_set_text(s_cardRoute, rt);
    } else {
        lv_label_set_text(s_cardRoute, "Looking up route...");     // pending: lookup in flight
    }

    // aircraft photo (planespotters), shown above the card when one is available
    if (in.hex[0]) photo_request(in.hex);
    int pw = 0, ph = 0; char pcred[40];
    if (s_photo && in.hex[0] && photo_get(in.hex, &pw, &ph, pcred, sizeof(pcred)) && pw > 0 && ph > 0) {
        int mw, mh;
        lv_color_t *pbuf = photo_buffer(&mw, &mh);
        lv_canvas_set_buffer(s_photo, pbuf, pw, ph, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_photo, pw, ph);
        lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -28 - ph / 2);   // sit lower: fill the band down to the card
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_photo);
        if (s_photoCredit) {
            char c[52];
            snprintf(c, sizeof(c), "Photo: %s", pcred[0] ? pcred : "planespotters.net");
            lv_label_set_text(s_photoCredit, c);
            lv_obj_align_to(s_photoCredit, s_photo, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_photo) {
        // No image to show yet: hide the canvas, but use the caption line to tell the
        // user what's happening — "Loading..." while the fetch is in flight, or a quiet
        // "No photo" once it finished without one. Unobtrusive (small, dim) but informative.
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) {
            const bool done = in.hex[0] && photo_done(in.hex);
            lv_label_set_text(s_photoCredit, done ? "No photo available" : "Loading photo...");
            lv_obj_align(s_photoCredit, LV_ALIGN_CENTER, 0, -104);   // where the photo would sit
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --------------------------------------------------------------------- input
static bool s_longPressed = false;
static int s_rangeIdx = -1;
static float s_rangeKm = RANGE_KM_DEFAULT;   // current display range (km), for the stats view
static void (*s_rangeCb)(float) = nullptr;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }

static void zoom_cb(lv_event_t *e) {   // fires on PRESS (robust vs scroll-cancel on the tileview)
    (void)e;
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;      // debounce repeated/held presses
    last = now;
    if (!s_rangeCb) return;
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    s_rangeIdx = (s_rangeIdx + 1) % n;
    s_rangeCb(RANGE_STEPS_KM[s_rangeIdx]);
}

void ui_set_range_km(float km) {
    s_rangeKm = km;
    if (s_zoomLbl) {
        char b[20];
        snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %.0f %s", dist_val(km), dist_unit());
        lv_label_set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) { float d = km - RANGE_STEPS_KM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

static void radar_press_cb(lv_event_t *e) { (void)e; s_longPressed = false; }

static void radar_longpress_cb(lv_event_t *e) {   // long-press cycles the visual theme
    (void)e;
    radar::cycleTheme();
    s_longPressed = true;
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_longPressed) { s_longPressed = false; return; }   // ignore the click after a long-press
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    radar::select(radar::hitTest(p.x, p.y));   // hit -> select; miss -> clear
    refresh_card();
}

static void list_btn_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    const int idx = (int)(intptr_t)lv_obj_get_user_data(b);
    radar::select(idx);
    refresh_card();
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);   // jump back to the radar
}

// ----------------------------------------------------------------- list/stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    // bar count from RSSI (dBm): the weaker the signal, the fewer lit bars
    int level;
    if      (!wifiUp)     level = 0;
    else if (rssi >= -55) level = 4;   // excellent
    else if (rssi >= -67) level = 3;   // good
    else if (rssi >= -75) level = 2;   // ok
    else                  level = 1;   // weak (connected but marginal)
    // colour: red = no WiFi, amber = connected but feed stale (no fresh data), white = healthy
    const lv_color_t col = !wifiUp ? UI_EMERG : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
    for (int i = 0; i < 4; ++i) {
        if (!s_hudBars[i]) continue;
        lv_obj_set_style_bg_color(s_hudBars[i], col, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
    if (!s_hudBatt) return;
    if (!present || pct < 0) { lv_label_set_text(s_hudBatt, ""); return; }   // USB-only -> hide
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                      pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 35 ? LV_SYMBOL_BATTERY_2 :
                      pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s%d", charging ? LV_SYMBOL_CHARGE : "", sym, pct);
    lv_label_set_text(s_hudBatt, buf);
    lv_obj_set_style_text_color(s_hudBatt, (pct <= 15 && !charging) ? UI_EMERG : UI_INK, 0);
}

void ui_set_date(const char *date) {
    if (s_hudDate && date) lv_label_set_text(s_hudDate, date);
}

void ui_set_netinfo(const char *line) {
    if (s_statsNet && line) lv_label_set_text(s_statsNet, line);
}

// GPS indicator. state: 0 = off / no module (hidden), 1 = acquiring (amber), 2 = fix (green).
void ui_set_gps(int state, int sats) {
    if (state <= 0) {                                 // hidden when GPS auto-location is off
        if (s_hudGps)   lv_label_set_text(s_hudGps, "");
        if (s_statsGps) lv_label_set_text(s_statsGps, "");
        return;
    }
    const bool fix = (state >= 2);
    const lv_color_t col = fix ? UI_GREEN : lv_color_hex(0xFFB23C);   // amber while acquiring
    if (s_hudGps) {
        char b[16];
        snprintf(b, sizeof(b), LV_SYMBOL_GPS "%d", sats);
        lv_label_set_text(s_hudGps, b);
        lv_obj_set_style_text_color(s_hudGps, col, 0);
    }
    if (s_statsGps) {
        char s[40];
        if (fix) snprintf(s, sizeof(s), LV_SYMBOL_GPS " fix  " LV_SYMBOL_BULLET "  %d sats", sats);
        else     snprintf(s, sizeof(s), LV_SYMBOL_GPS " acquiring  (%d sats)", sats);
        lv_label_set_text(s_statsGps, s);
        lv_obj_set_style_text_color(s_statsGps, col, 0);
    }
}

// Rebuild the scrollable contact list. Costly (deletes+recreates LVGL buttons), so we
// only call it when the list tile is actually visible — not on every 2 s poll.
static void build_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    const int n = radar::count();
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        char altS[16], txt[64];
        fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
        snprintf(txt, sizeof(txt), "%-8.8s  %-8s %4.1f %s",
                 in.call[0] ? in.call : in.hex, altS, dist_val(in.distKm), dist_unit());
        lv_obj_t *b = lv_list_add_btn(s_list, NULL, txt);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, in.emergency ? UI_EMERG : UI_SOFT, 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, list_btn_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void build_stats(void) {
    if (!s_statsLbl) return;
    const int n = radar::count();
    int emg = 0;
    float nearest = 1e9f, highest = -1e9f;
    char nearestCall[12] = "-";
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        if (in.emergency) emg++;
        if (in.distKm < nearest) { nearest = in.distKm; snprintf(nearestCall, sizeof(nearestCall), "%s", in.call[0] ? in.call : in.hex); }
        if (!in.onGround && in.altFt > highest) highest = in.altFt;
    }
    char altH[16];
    fmt_alt(altH, sizeof(altH), (highest > -1e8f) ? highest : 0.0f, false);
    char st[220];
    snprintf(st, sizeof(st),
             "Aircraft   %d\n"
             "Emergency  %d\n"
             "Nearest    %s\n"
             "           %.1f %s\n"
             "Highest    %s\n"
             "Range      %.0f %s",
             n, emg, n ? nearestCall : "-", dist_val(n ? nearest : 0.0f), dist_unit(),
             altH, dist_val(s_rangeKm), dist_unit());
    lv_label_set_text(s_statsLbl, st);
}

// Rebuild whichever of list/stats is currently on screen (called on poll and on swipe).
static void refresh_active_tile(void) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)  build_list();
    else if (act == s_tileStats) build_stats();
}

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%d", radar::countInRange());
        lv_label_set_text(s_hudCount, cbuf);
    }
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// ------------------------------------------------------------------- building
static lv_obj_t *make_tile_title(lv_obj_t *tile, const char *txt) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, UI_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

// A full-screen round panel that clips its content to the circle (for list/stats views).
static lv_obj_t *make_round_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, UI_GREEN, 0);
    lv_obj_set_style_border_opa(p, 50, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_card(void) {
    s_card = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 300, 118);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 66);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, 235, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_color(s_card, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_card, 90, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cardTitle, UI_INK, 0);
    lv_obj_align(s_cardTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cardL = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardL, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardL, UI_SOFT, 0);
    lv_obj_align(s_cardL, LV_ALIGN_TOP_LEFT, 0, 26);

    s_cardR = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardR, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardR, UI_SOFT, 0);
    lv_obj_align(s_cardR, LV_ALIGN_TOP_LEFT, 150, 26);

    s_cardRoute = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardRoute, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardRoute, UI_GREEN, 0);
    lv_obj_align(s_cardRoute, LV_ALIGN_TOP_LEFT, 0, 76);

    // aircraft photo + credit, floating above the card (hidden until one loads)
    s_photo = lv_canvas_create(s_tileRadar);
    lv_obj_set_style_radius(s_photo, 6, 0);
    lv_obj_set_style_clip_corner(s_photo, true, 0);
    lv_obj_set_style_border_color(s_photo, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_photo, 170, 0);
    lv_obj_set_style_border_width(s_photo, 1, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_photoCredit = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_photoCredit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_photoCredit, UI_DIM, 0);
    lv_label_set_text(s_photoCredit, "");
    lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_view(int idx) {
    if (s_tv && idx >= 0 && idx <= 2) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

// JPEG splash decode target (used by splash_jpg_out callback)
#if defined(ESP_PLATFORM)
static lv_color_t *s_splashBuf = nullptr;
static int s_splashW = 0, s_splashH = 0;
static lv_img_dsc_t s_splashDsc;

static bool splash_jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < (int)h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= s_splashH) continue;
        for (int i = 0; i < (int)w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= s_splashW) continue;
            s_splashBuf[yy * s_splashW + xx].full = bmp[j * w + i];
        }
    }
    return true;
}
#endif

static void splash_del_free_cb(lv_anim_t *a) {
    lv_obj_del((lv_obj_t *)a->var);
#if defined(ESP_PLATFORM)
    if (s_splashBuf) { heap_caps_free(s_splashBuf); s_splashBuf = nullptr; }
#endif
}

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_free_cb);
    lv_anim_start(&a);
}

void ui_splash_show(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

#if defined(ESP_PLATFORM)
    // Decode the embedded JPEG splash into a PSRAM buffer and show it as an LVGL image.
    s_splashW = SCREEN_W;
    s_splashH = SCREEN_H;
    const size_t bufSz = (size_t)s_splashW * s_splashH * sizeof(lv_color_t);
    s_splashBuf = (lv_color_t *)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM);
    bool imgOk = false;
    if (s_splashBuf) {
        memset(s_splashBuf, 0, bufSz);
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(splash_jpg_out);
        imgOk = (TJpgDec.drawJpg(0, 0, splash_jpg_data, splash_jpg_len) == JDR_OK);
    }
    if (imgOk) {
        s_splashDsc.header.always_zero = 0;
        s_splashDsc.header.w = s_splashW;
        s_splashDsc.header.h = s_splashH;
        s_splashDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_splashDsc.data_size = bufSz;
        s_splashDsc.data = (const uint8_t *)s_splashBuf;
        lv_obj_t *img = lv_img_create(cont);
        lv_img_set_src(img, &s_splashDsc);
        lv_obj_center(img);
    } else {
        if (s_splashBuf) { heap_caps_free(s_splashBuf); s_splashBuf = nullptr; }
#endif
        // Fallback: procedural splash (simulator or decode failure)
        const lv_coord_t dia[3] = { 210, 142, 78 };
        const lv_opa_t   op[3]  = { 90, 120, 160 };
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *r = lv_obj_create(cont);
            lv_obj_remove_style_all(r);
            lv_obj_set_size(r, dia[i], dia[i]);
            lv_obj_align(r, LV_ALIGN_CENTER, 0, -8);
            lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_color(r, UI_GREEN, 0);
            lv_obj_set_style_border_opa(r, op[i], 0);
            lv_obj_set_style_border_width(r, 2, 0);
            lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_t *sweep = lv_spinner_create(cont, 1400, 55);
        lv_obj_set_size(sweep, 210, 210);
        lv_obj_align(sweep, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_arc_opa(sweep, 0, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sweep, UI_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(sweep, 4, LV_PART_INDICATOR);

        lv_obj_t *title = lv_label_create(cont);
        lv_label_set_text(title, "CAPSULE RADAR");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(title, UI_GREEN, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 118);

        lv_obj_t *sub = lv_label_create(cont);
        lv_label_set_text(sub, "Live ADS-B radar");
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sub, UI_SOFT, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 150);
#if defined(ESP_PLATFORM)
    }
#endif

    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2200, cont);
    lv_timer_set_repeat_count(t, 1);
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tileRadar = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileList  = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT);
    // Rebuild the list/stats with the latest data the moment they slide into view
    // (between polls they'd otherwise show whatever was there when last visible).
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) { refresh_active_tile(); }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom button shows the range instead
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (planes/empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_card();

    // on-screen range/zoom button (reliable single tap; bottom, above the 'S' marker)
    s_zoomBtn = lv_btn_create(s_tileRadar);
    lv_obj_set_size(s_zoomBtn, 120, 44);
    lv_obj_set_ext_click_area(s_zoomBtn, 18);   // invisibly enlarge the tap target (easier to hit)
    lv_obj_align(s_zoomBtn, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_radius(s_zoomBtn, 18, 0);
    lv_obj_set_style_bg_color(s_zoomBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_zoomBtn, 225, 0);
    lv_obj_set_style_border_color(s_zoomBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_zoomBtn, 1, 0);
    lv_obj_set_style_border_opa(s_zoomBtn, 170, 0);
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_PRESSED, NULL);  // fire on touch-down, not release
    s_zoomLbl = lv_label_create(s_zoomBtn);
    lv_label_set_text(s_zoomLbl, LV_SYMBOL_LOOP " 30 km");
    lv_obj_set_style_text_font(s_zoomLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_zoomLbl, UI_GREEN, 0);
    lv_obj_center(s_zoomLbl);

    // top status HUD (wifi / aircraft count / clock); white reads on both themes.
    // WiFi is a 4-bar signal meter: bar count = RSSI strength, colour = feed health.
    s_hudWifi = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_hudWifi);
    lv_obj_set_size(s_hudWifi, 21, 14);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -94, 50);
    lv_obj_clear_flag(s_hudWifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_hudBars[i] = lv_obj_create(s_hudWifi);
        lv_obj_remove_style_all(s_hudBars[i]);
        lv_obj_set_size(s_hudBars[i], 3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
        lv_obj_align(s_hudBars[i], LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(i * 5), 0);
        lv_obj_set_style_radius(s_hudBars[i], 1, 0);
        lv_obj_set_style_bg_color(s_hudBars[i], UI_INK, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_hudBars[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_hudGps = lv_label_create(s_tileRadar);     // GPS satellite icon (between WiFi bars and count)
    lv_obj_set_style_text_font(s_hudGps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudGps, UI_GREEN, 0);
    lv_label_set_text(s_hudGps, "");             // hidden until ui_set_gps() says GPS is on
    lv_obj_align(s_hudGps, LV_ALIGN_TOP_MID, -62, 50);

    s_hudCount = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudCount, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudCount, UI_INK, 0);
    lv_label_set_text(s_hudCount, "0");
    lv_obj_align(s_hudCount, LV_ALIGN_TOP_MID, -34, 50);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 30, 50);

    s_hudBatt = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudBatt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 92, 50);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hudDate, UI_INK, 0);
    lv_obj_set_style_text_opa(s_hudDate, 140, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 70);

    // --- list tile (circular panel, clipped to the round screen) ---
    lv_obj_t *lp = make_round_panel(s_tileList);
    make_tile_title(lp, "AIRCRAFT");
    s_list = lv_list_create(lp);
    lv_obj_set_size(s_list, 300, 372);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);

    // --- stats tile (circular panel) ---
    lv_obj_t *sp = make_round_panel(s_tileStats);
    make_tile_title(sp, "STATS");
    s_statsLbl = lv_label_create(sp);
    lv_obj_set_style_text_font(s_statsLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_statsLbl, UI_SOFT, 0);
    lv_label_set_text(s_statsLbl, "Aircraft   0");
    lv_obj_align(s_statsLbl, LV_ALIGN_CENTER, 0, -16);

    s_statsGps = lv_label_create(sp);               // GPS status line (hidden unless GPS is on)
    lv_obj_set_style_text_font(s_statsGps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsGps, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_statsGps, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsGps, "");
    lv_obj_align(s_statsGps, LV_ALIGN_CENTER, 0, 90);

    // footer: where to reach the configuration page (IP / hostname / setup AP)
    s_statsNet = lv_label_create(sp);
    lv_obj_set_width(s_statsNet, 320);
    lv_obj_set_style_text_font(s_statsNet, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsNet, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsNet, "");
    lv_obj_align(s_statsNet, LV_ALIGN_CENTER, 0, 132);

    lv_obj_t *ver = lv_label_create(sp);            // firmware version (so users can tell what's flashed)
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ver, UI_DIM, 0);
    lv_label_set_text(ver, "Capsule Radar v" FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 170);

    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);

    ui_splash_show();   // branded boot splash on top (auto-fades)
}
