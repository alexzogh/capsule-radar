// Clock face idle-mode overlay. Sits on lv_layer_top() so it covers the
// radar/list/stats tileview without modifying the tileview structure.
// Visual: phosphor-green on true black, matching the radar palette.
#include "ui_clock.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

// ---- colors (match radar_view / ui.cpp palette) ----
#define CK_GREEN  lv_color_hex(0x1DFF86)
#define CK_SOFT   lv_color_hex(0x9AFFC8)
#define CK_DIM    lv_color_hex(0x5F7A6C)

// ---- widgets ----
static lv_obj_t  *s_cont      = nullptr;   // full-screen overlay container
static lv_obj_t  *s_lblTime   = nullptr;   // HH:MM
static lv_obj_t  *s_lblSec    = nullptr;   // :SS
static lv_obj_t  *s_lblDate   = nullptr;   // e.g. "Tue 15 Jul"
static lv_obj_t  *s_lblAc     = nullptr;   // aircraft count
static lv_obj_t  *s_lblNearest = nullptr;  // closest aircraft info
static lv_obj_t  *s_ring      = nullptr;   // decorative outer ring
static lv_obj_t  *s_sweep     = nullptr;   // very slow, faint sweep spinner
static lv_timer_t *s_timer    = nullptr;   // 1 s tick while visible

// ---- cached nearest-aircraft data (set by clock_update, read by the timer) ----
static int   s_acCount = 0;
static char  s_nearCall[16] = "";
static float s_nearDist = 0.0f;
static float s_nearAlt  = 0.0f;

// ---- timer callback: refresh time + aircraft info every second ----
static void clock_tick_cb(lv_timer_t *) {
    time_t now = time(nullptr);
    struct tm ti;
    if (now > 1000000000L && localtime_r(&now, &ti)) {
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", ti.tm_hour, ti.tm_min);
        lv_label_set_text(s_lblTime, hm);

        char sec[8];
        snprintf(sec, sizeof(sec), ":%02d", ti.tm_sec);
        lv_label_set_text(s_lblSec, sec);

        // e.g. "Tue 15 Jul"
        char date[24];
        strftime(date, sizeof(date), "%a %d %b", &ti);
        lv_label_set_text(s_lblDate, date);
    }

    // aircraft info
    if (s_acCount > 0) {
        char ac[24];
        snprintf(ac, sizeof(ac), "%d aircraft", s_acCount);
        lv_label_set_text(s_lblAc, ac);

        if (s_nearCall[0]) {
            char nr[64];
            snprintf(nr, sizeof(nr), "%s  %.1f km  %.0f ft", s_nearCall, s_nearDist, s_nearAlt);
            lv_label_set_text(s_lblNearest, nr);
        } else {
            lv_label_set_text(s_lblNearest, "");
        }
    } else {
        lv_label_set_text(s_lblAc, "No aircraft");
        lv_label_set_text(s_lblNearest, "");
    }
}

void clock_create() {
    // Full-screen overlay on lv_layer_top() (same approach as the splash screen)
    s_cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_size(s_cont, SCREEN_W, SCREEN_H);
    lv_obj_center(s_cont);
    lv_obj_set_style_bg_color(s_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    // Consume all touch events so they don't reach the tileview underneath
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_CLICKABLE);

    // Decorative outer ring (thin green circle near the edge of the round screen)
    const lv_coord_t ringDia = (SCREEN_W < SCREEN_H ? SCREEN_W : SCREEN_H) - 12;
    s_ring = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, ringDia, ringDia);
    lv_obj_center(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_ring, CK_GREEN, 0);
    lv_obj_set_style_border_opa(s_ring, 60, 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Very slow, very faint sweep spinner (one rotation per 30 s = 30000 ms)
    s_sweep = lv_spinner_create(s_cont, 30000, 50);
    lv_obj_set_size(s_sweep, ringDia - 8, ringDia - 8);
    lv_obj_center(s_sweep);
    lv_obj_set_style_arc_opa(s_sweep, 0, LV_PART_MAIN);              // hide the track arc
    lv_obj_set_style_arc_color(s_sweep, CK_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_sweep, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_sweep, 40, LV_PART_INDICATOR);        // very faint

    // Large time: HH:MM
    s_lblTime = lv_label_create(s_cont);
    lv_label_set_text(s_lblTime, "--:--");
    lv_obj_set_style_text_font(s_lblTime, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lblTime, CK_GREEN, 0);
    lv_obj_set_style_text_letter_space(s_lblTime, 4, 0);
    lv_obj_align(s_lblTime, LV_ALIGN_CENTER, -10, -40);

    // Seconds (to the right of the time)
    s_lblSec = lv_label_create(s_cont);
    lv_label_set_text(s_lblSec, "");
    lv_obj_set_style_text_font(s_lblSec, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lblSec, CK_SOFT, 0);
    lv_obj_set_style_text_opa(s_lblSec, 180, 0);
    lv_obj_align_to(s_lblSec, s_lblTime, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);

    // Date line
    s_lblDate = lv_label_create(s_cont);
    lv_label_set_text(s_lblDate, "");
    lv_obj_set_style_text_font(s_lblDate, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lblDate, CK_SOFT, 0);
    lv_obj_set_style_text_opa(s_lblDate, 180, 0);
    lv_obj_align(s_lblDate, LV_ALIGN_CENTER, 0, 16);

    // Aircraft count
    s_lblAc = lv_label_create(s_cont);
    lv_label_set_text(s_lblAc, "");
    lv_obj_set_style_text_font(s_lblAc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lblAc, CK_DIM, 0);
    lv_obj_align(s_lblAc, LV_ALIGN_CENTER, 0, 62);

    // Nearest aircraft info
    s_lblNearest = lv_label_create(s_cont);
    lv_label_set_text(s_lblNearest, "");
    lv_obj_set_style_text_font(s_lblNearest, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lblNearest, CK_DIM, 0);
    lv_obj_set_style_text_opa(s_lblNearest, 160, 0);
    lv_obj_align(s_lblNearest, LV_ALIGN_CENTER, 0, 90);

    // Start hidden
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
}

void clock_update(int aircraftCount, const char *nearestCall,
                  float nearestDistKm, float nearestAltFt) {
    s_acCount = aircraftCount;
    if (nearestCall) snprintf(s_nearCall, sizeof(s_nearCall), "%s", nearestCall);
    else s_nearCall[0] = '\0';
    s_nearDist = nearestDistKm;
    s_nearAlt  = nearestAltFt;
}

void clock_show() {
    if (!s_cont) return;
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
    // Immediate tick so the clock shows the current time right away
    clock_tick_cb(nullptr);
    lv_obj_align_to(s_lblSec, s_lblTime, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
    // Start the 1 s timer
    if (!s_timer) {
        s_timer = lv_timer_create(clock_tick_cb, 1000, nullptr);
    }
}

void clock_hide() {
    if (!s_cont) return;
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
}

bool clock_visible() {
    if (!s_cont) return false;
    return !lv_obj_has_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
}
