#pragma once
// Capsule Radar — build & user configuration.
//
// This firmware targets two Waveshare round-display boards, selected at build time:
//   BOARD_AMOLED175 (default) — ESP32-S3-Touch-AMOLED-1.75 : CO5300 AMOLED 466x466, QSPI, CST9217 touch.
//   BOARD_LCD21               — ESP32-S3-Touch-LCD-2.1      : ST7701 IPS 480x480, RGB parallel, CST820 touch.
// The board macro comes from platformio.ini (build_flags -D...). Everything above the
// board split is common; hardware-specific pins/geometry live in the #if blocks below.

#define FW_VERSION "1.4.0"   // shown on the web config page + Stats screen; bump on release

// Default to the original AMOLED board if no board was selected by the build.
#if !defined(BOARD_LCD21) && !defined(BOARD_AMOLED175) && !defined(BOARD_AMOLED_143)
#  define BOARD_AMOLED175
#endif

// ---------- Home location (default: Dénia, Spain) ----------
// Overridable at runtime via the captive portal (stored in NVS).
#define HOME_LAT_DEFAULT   38.8409
#define HOME_LON_DEFAULT    0.1059

// ---------- Radar ----------
#define RANGE_KM_DEFAULT    30.0f          // display range (outer ring). Query is wider, see below.
// Feed query radius = display range × MULT, clamped to [MIN, MAX]. Querying a bit wider than
// the display shows off-range traffic as edge arrows. The floor MUST stay small: the old 50 km
// floor made small display ranges still pull a huge aircraft list in busy airspace, which timed
// out the poll (feed permanently amber near big hubs). Fix contributed by @alexzogh (STLWarehouse).
#define ADSB_QUERY_MULT     1.4f
#define ADSB_QUERY_MIN_KM   12.0f
#define ADSB_QUERY_MAX_KM   150.0f
static const float RANGE_STEPS_KM[] = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
#define POLL_INTERVAL_MS    2000           // be gentle with the free API (>=1000)
#define POLL_INTERVAL_BATTERY_MS 5000      // slower polling when running on battery
// Reboot to recover if the feed is stuck this long with WiFi up (heap-fragmentation safety
// net). Much longer on the RGB board: a transient API rate-limit must NOT trigger a reboot
// loop there, since a reboot drops the WiFi-setup portal session. 0 = never auto-reboot.
#if defined(BOARD_LCD21)
#  define FEED_STUCK_REBOOT_MS 900000      // 15 min
#else
#  define FEED_STUCK_REBOOT_MS 180000      // 3 min (AMOLED, original behaviour)
#endif
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos
#define AC_STALE_MS         15000          // keep the last contacts through brief empty feed responses

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

// ---------- ADS-B API (free, non-commercial) ----------
// Providers are tried in this order; each is paced independently (see AdsbClient).
// All three return the same readsb shape (an "ac" array), but NOT the same URL path,
// so each carries its own template in the provider table in adsb_client.cpp.
#define ADSB_PRIMARY_HOST   "api.airplanes.live"   // GET /v2/point/{lat}/{lon}/{radius_nm}
                                                    //   now requires prior approval by email
#define ADSB_OPENDATA_HOST  "opendata.adsb.fi"     // GET /api/v3/lat/{lat}/lon/{lon}/dist/{nm}
                                                    //   documented 1 req/s; personal use only,
                                                    //   attribution required (see docs/DATA_SOURCE.md)
#define ADSB_FALLBACK_HOST  "api.adsb.lol"          // same readsb format; limits are dynamic
#define ADSB_PROVIDER_COUNT 3
// Sent with setUserAgent(), never addHeader() — see the note in docs/DATA_SOURCE.md.
// Carries FW_VERSION: now that the header actually goes out, and adsb.lol checks it for
// valid contact info, a provider should be able to tell which build is talking to them.
#define ADSB_USER_AGENT     "CapsuleRadar/" FW_VERSION " (ESP32-S3 hobby; +https://github.com/socquique/capsule-radar)"
#define ADSB_HTTPS_INSECURE 1               // 1 = setInsecure() (hobby). 0 = use pinned root CA.
#define ADSB_MAX_AIRCRAFT   60              // hard cap parsed per poll (protect RAM in busy areas)
// How long to stop asking a provider that refused us. 403 is a policy refusal (needs
// approval, or a User-Agent they reject) and will not clear in seconds; 429 just means
// we were too fast. Without these, a permanently-403 provider is retried every poll,
// which doubles the request rate onto the surviving provider and trips ITS rate limit.
#define ADSB_COOLDOWN_403_MS  900000UL      // 15 min park after a policy refusal
#define ADSB_SPACING_STEP_MS    4000UL      // first extra gap imposed after a 429
#define ADSB_SPACING_MAX_MS    60000UL      // never space a provider out further than this
#define ADSB_SPACING_EASE_OKS       3       // successes needed before easing the gap back down
#define ADSB_FEED_STALE_MS     60000UL      // no successful fetch for this long -> HUD warning

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Board ----------
// The pin map, panel gaps, touch driver and which peripherals exist all live in a
// per-board header (or inline, for the LCD 2.1). Select one with a build flag in
// platformio.ini; the 1.75 is the default so an unflagged build behaves exactly as before.
//   -DBOARD_LCD21       -> Waveshare ESP32-S3-Touch-LCD-2.1
//   -DBOARD_AMOLED_143  -> Waveshare ESP32-S3-Touch-AMOLED-1.43
//   (none)              -> Waveshare ESP32-S3-Touch-AMOLED-1.75  (reference board)
// Never guess pins for a new board: take them from the vendor demo or the Arduino
// core board variant, then confirm them on hardware before committing.
#if defined(BOARD_LCD21)
// ---------------------------------------------------------------------------
//  Waveshare ESP32-S3-Touch-LCD-2.1 : ST7701 480x480 IPS, 16-bit RGB parallel.
//  Pins/timings are from Waveshare's own Arduino demo (LVGL_Arduino):
//    files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Code.zip
//  The verbose RGB data-bus pin table + ST7701 init sequence live in display_st7701.cpp
//  (lifted verbatim from the demo). Only the higher-level knobs are here.
// ---------------------------------------------------------------------------

// ---------- Screen (ST7701 IPS, 480x480 round) ----------
#define SCREEN_W            480
#define SCREEN_H            480
#define SCREEN_CX           240
#define SCREEN_CY           240
#define RADAR_R_OUTER_PX    236            // outer ring radius in px (fills the 480 round panel)

// ---------- Backlight (PWM on a real GPIO — unlike the AMOLED's panel command) ----------
#define PIN_LCD_BL          6              // ledc PWM backlight enable/level
#define LCD_BL_PWM_FREQ     20000          // 20 kHz (from the demo)
#define LCD_BL_PWM_BITS     10             // 10-bit resolution -> duty 0..1023

// ---------- 3-wire SPI used only to push the ST7701 init sequence ----------
#define PIN_LCD_SCLK        2              // also referenced by main.cpp's pin sanity print
#define PIN_LCD_MOSI        1

// ---------- Shared I2C bus (touch + TCA9554 expander + QMI8658 IMU + PCF85063 RTC) ----------
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         7

// ---------- TCA9554PWR I2C GPIO expander (drives lines the ESP32 GPIOs don't) ----------
#define TCA9554_ADDR        0x20
#define EXIO_LCD_RST        1              // ST7701 reset   (EXIO1)
#define EXIO_TP_RST         2              // CST820 reset   (EXIO2)
#define EXIO_LCD_CS         3              // ST7701 3-wire SPI chip-select (EXIO3)

// ---------- Touch (CST820 capacitive, I2C) ----------
#define PIN_TP_INT          16             // CST820 interrupt (real GPIO); reset is EXIO_TP_RST
#define I2C_ADDR_TOUCH      0x15
#define TP_MIRROR_X         false          // adjust on-hardware if X is flipped
#define TP_MIRROR_Y         false          // adjust on-hardware if Y is flipped

// ---------- Audio: this board has NO onboard codec. Stubbed (audio_begin() self-disables). ----------
#define PIN_AUDIO_PA        -1             // no speaker amp -> pinMode(-1) is a safe no-op
#define PIN_I2S_MCLK        -1
#define PIN_I2S_BCLK        -1
#define PIN_I2S_LRCLK       -1
#define PIN_I2S_DOUT        -1
#define PIN_I2S_DIN         -1

// ---------- I2C addresses (shared bus) ----------
#define I2C_ADDR_IMU        0x6B           // QMI8658 (imu driver also probes 0x6A)
#define I2C_ADDR_RTC        0x51           // PCF85063
// (no AXP2101 PMIC on this board — battery_begin() reports "not found" and no-ops)

#define PIN_BOOT_BUTTON     0              // BOOT button

#elif defined(BOARD_AMOLED_143)
#  include "boards/board_amoled_143.h"
#else
#  include "boards/board_amoled_175.h"
#endif

// ---------- Screen geometry (board-specific where panels differ) ----------
#if !defined(SCREEN_W)
// AMOLED boards: CO5300 466x466.
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#endif

// ---------- Display brightness / idle ----------
#if !defined(BRIGHTNESS_DEFAULT)
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#endif
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         20000          // dim the screen after this long without a touch
#define TZ_STR              "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ (Spain) for local time/date

// Safety net: catches a board header that still has placeholder pins in it.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "board header: QSPI/I2C pins are placeholders (-1). Fill in the real values."
#endif
