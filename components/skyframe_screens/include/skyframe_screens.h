#pragma once
#include "lvgl.h"
#include "skyframe_net.h"
#include "skyframe_engine.h"
#include <stdbool.h>
#include <stdint.h>

/* All functions must be called from the LVGL task or under the display mutex. */
void sf_screens_init(lv_obj_t *parent);
void sf_weather_show(bool show);
void sf_photo_show(bool show);
/* Current conditions, today's range, details and the next SF_WEATHER_HOURS hours. */
void sf_weather_update(const sf_weather_t *weather);
/* Advance one photo every 10 seconds, when in slideshow mode. */
void sf_photo_tick(uint32_t now_ms);
/* Why the slideshow has nothing new to show (shown until the first photo appears). */
typedef enum {
    SF_PHOTOS_LOADING,      /* decoding the next photo */
    SF_PHOTOS_OK,
    SF_PHOTOS_NO_CARD,
    SF_PHOTOS_NONE,         /* card mounted, but no .jpg in /photos */
    SF_PHOTOS_UNREADABLE,   /* every photo failed to decode */
    SF_PHOTOS_USB,          /* the card is connected to a computer */
} sf_photo_status_t;
/* Implement callback to provide successive predecoded frames without blocking.
   NULL keeps the current image (next frame not ready yet); *status says why. */
typedef const lv_img_dsc_t *(*sf_next_photo_cb_t)(void *ctx, sf_photo_status_t *status);
void sf_photo_set_provider(sf_next_photo_cb_t cb, void *ctx);

/* Setup / settings screen: QR code to join the setup network, its name and password,
   and (when connected) the device's address on the home network. */
typedef struct {
    const char *title;
    const char *subtitle;    /* one line under the title (e.g. the Wi-Fi name); may be NULL */
    const char *ap_ssid, *ap_password;
    const char *home_ip;     /* NULL when not on the home network */
    bool closable;           /* Close/Done buttons (settings opened by a 5 s hold) */
} sf_setup_info_t;
void sf_setup_show(bool show, const sf_setup_info_t *info);
void sf_screens_set_setup_close_cb(void (*cb)(void));

/* Gestures: long press (next screen) and 5 s hold (settings). Attach to any
   full-screen page; callbacks are shared. */
void sf_gesture_attach(lv_obj_t *obj);
/* For clickable children that keep their own presses (radar planes): note where a press
   starts, so a swipe down beginning on them opens the panel correctly. */
void sf_gesture_track_press(lv_obj_t *obj);
void sf_screens_set_hold_cb(void (*cb)(void));

/* Verse of the day page. */
void sf_verse_set(const sf_verse_t *verse);
void sf_verse_show(bool show);

/* Birthday reminder page. pixels: photo_size^2 RGB565 (180) or NULL for an initial;
   it must stay valid until the next sf_birthday_set(). */
typedef struct {
    const char *name;
    int age;                 /* -1 unknown */
    int month, day;
    bool owner;              /* special screen for the device's owner */
    const char *message;     /* owner's personal message */
    const uint16_t *pixels;
    int photo_size;
} sf_birthday_view_t;
void sf_birthday_set(const sf_birthday_view_t *view);
void sf_birthday_show(bool show);
void sf_screens_set_birthday_dismiss_cb(void (*cb)(void));

/* Test mode: opened by holding the settings screen for 5 s. */
typedef enum {
    SF_TEST_OWNER_BIRTHDAY, SF_TEST_FAMILY_BIRTHDAY, SF_TEST_TRAFFIC, SF_TEST_NIGHT,
    SF_TEST_TOUR, SF_TEST_USB, SF_TEST_WIFI, SF_TEST_WX_ALERT, SF_TEST_SIGHTING, SF_TEST_TODAY,
    SF_TEST_OVERHEAD, SF_TEST_SPLASH,
    SF_TEST_RESTART, SF_TEST_EXIT, SF_TEST_COUNT,
    SF_TEST_STOP,         /* not a menu item: the badge was tapped, back to the menu */
    SF_TEST_NEXT          /* not a menu item: "Next birthday" during the family birthday test */
} sf_test_action_t;
void sf_test_menu_show(bool show);
/* System panel and web page addresses on the test menu. Unknown values: -1 (ints),
   NAN (floats) or "" (strings). */
typedef struct {
    int cpu[2];                    /* percent busy per core */
    unsigned ram_kb, ram_low_kb;   /* internal RAM free now / lowest since boot */
    unsigned ram_block_kb;         /* largest free internal block */
    float psram_free_mb, psram_total_mb;
    float sd_free_gb, sd_total_gb; /* NAN: no card, or it is on a computer */
    float chip_c;                  /* chip temperature (runs warmer than the room) */
    float battery_v;
    int rssi, channel;             /* rssi 0: not connected */
    char ip[16];
    unsigned cpu_mhz;
    uint32_t uptime_s, boots;
    const char *reset_reason;      /* why the last restart happened, in plain words */
    char version[64];              /* header: "SkyFrame v0.4  *  build 6770179  *  Nov 23 2026" */
    char built[32];                /* "Nov 23 2026 14:02:11" */
} sf_system_stats_t;
void sf_test_menu_set_stats(const sf_system_stats_t *stats);
void sf_screens_set_test_cb(void (*enter)(void), void (*action)(sf_test_action_t action));
/* Small banner above everything while a test runs; NULL hides it. Tapping it stops the test. */
void sf_test_badge(const char *text);
/* "Next birthday" button at the bottom while the family birthday test runs. */
void sf_test_next(bool show);

/* Weather page clock ("10:45 PM"; empty hides it). */
void sf_weather_set_clock(const char *text);

/* Severe-weather alerts on the weather page, most serious first (count 0 removes the card).
   Strings are copied. Tapping the card opens the full text over the page. */
typedef struct {
    const char *event;      /* "Severe Thunderstorm Warning" */
    const char *until;      /* "Until 6:45 PM", or "" */
    const char *area;       /* may be "" */
    const char *text;       /* paragraphs separated by '\n' */
    uint8_t level;          /* 3 warning, 2 watch, 1 advisory, 0 other */
} sf_weather_alert_view_t;
void sf_weather_set_alerts(const sf_weather_alert_view_t *alerts, int count, int total);
/* True while an alert's full text is open (the rotation waits). */
bool sf_weather_alert_open(void);
void sf_weather_close_alert(void);

/* "Today's sky" daily summary page. Strings are copied; NULL or "" leaves a part out. */
typedef struct {
    const char *date;               /* "Sunday, November 23" */
    unsigned planes;
    uint16_t per_hour[24];
    int now_hour;                   /* highlighted bar; -1 none */
    const char *highest, *highest_who;   /* "38,000 ft", "UAL123  B738" */
    const char *closest, *closest_who;
    const char *fastest, *fastest_who;
    const char *rarest;             /* unused by the page (kept for callers) */
    const char *special;            /* "2 military  *  1 VIP" */
    const char *special_who;        /* "SAM44, RCH451" */
    const char *hero_tag;           /* over the photo: "RAREST TODAY" */
    const char *hero_name;          /* "Boeing 747-8F" */
    const char *hero_who;           /* "CLX4421" */
} sf_today_view_t;
void sf_today_set(const sf_today_view_t *view);
/* The hero photo (SKYFRAME_PHOTO_W x SKYFRAME_PHOTO_H RGB565, 280x156), or NULL for the
   drawn plane. The page takes ownership and frees it when replaced; false: not taken. */
bool sf_today_set_photo(uint16_t *pixels);
void sf_today_show(bool show);

/* Sighting alert: a banner above every screen with a pulse around the rim; it leaves by
   itself after `seconds`. NULL hides it at once. Tapping it calls the tap callback. */
/* Least to most important. Overhead: a plane passing within about a mile (shorter, one pulse). */
typedef enum { SF_SIGHTING_OVERHEAD, SF_SIGHTING_TYPE, SF_SIGHTING_MILITARY, SF_SIGHTING_VIP,
               SF_SIGHTING_EMERGENCY } sf_sighting_kind_t;
typedef struct {
    sf_sighting_kind_t kind;
    const char *title;      /* "AIR FORCE ONE" */
    const char *name;       /* "AF1  *  Boeing VC-25A" */
    const char *detail;     /* "8 mi NE  *  21,000 ft" */
    unsigned seconds;
} sf_sighting_view_t;
void sf_sighting_show(const sf_sighting_view_t *view);

/* Pull-down panel: swipe down from the top third of any rotation screen. Brightness,
   shortcuts to the screens, pause the rotation, USB file transfer, settings. Closes on a swipe up, a tap below
   it, or after 20 s untouched. */
typedef enum { SF_QUICK_RADAR, SF_QUICK_WEATHER, SF_QUICK_PHOTOS, SF_QUICK_TODAY, SF_QUICK_PAUSE,
               SF_QUICK_SETTINGS, SF_QUICK_USB, SF_QUICK_TRANSITIONS } sf_quick_action_t;
typedef struct {
    int brightness;          /* percent of SkyFrame's automatic level, 10-100 */
    bool paused;
    bool today_available;    /* Today's sky has something to show */
    bool usb_available;      /* a computer is on the USB port: file transfer can start */
    bool transitions;        /* page transition animations on (the switch) */
    sf_screen_t screen;      /* highlighted shortcut */
    const char *status;      /* "4:52 PM  *  Wi-Fi strong" */
} sf_quick_state_t;
/* brightness: called while the slider moves (done=false) and on release (done=true). */
void sf_quick_set_callbacks(void (*brightness)(int percent, bool done), void (*action)(sf_quick_action_t a));
void sf_quick_update(const sf_quick_state_t *state);
bool sf_quick_is_open(void);
void sf_quick_close(void);

/* Animated changes between rotation screens. The book first closes over the old screen
   (wait for sf_transition_ready()); the rest cut a snapshot of the old screen away and are
   ready at once. Then switch screens and call sf_transition_reveal().
   SWEEP: the radar's last sweep wipes it away. FLYOVER: a jet crosses at 45 degrees.
   PING: a radar ring opens. SUNRISE: a sun rises from below. CONFETTI: an iris ringed with
   confetti. PAGE: a page turn. BOOK: a book closes and opens. FADE: a cross-fade. */
typedef enum { SF_TRANSITION_NONE, SF_TRANSITION_FADE, SF_TRANSITION_SWEEP, SF_TRANSITION_PAGE,
               SF_TRANSITION_BOOK, SF_TRANSITION_FLYOVER, SF_TRANSITION_PING, SF_TRANSITION_SUNRISE,
               SF_TRANSITION_CONFETTI } sf_transition_t;
bool sf_transition_begin(sf_transition_t kind);   /* false: none started (switch at once) */
/* Where the old screen is copied from: the panel's frame buffer (480x480 RGB565). */
void sf_screens_set_frame_source(const void *(*frame)(void));
bool sf_transition_ready(void);
bool sf_transition_busy(void);
void sf_transition_reveal(void);
void sf_transition_abort(void);                    /* e.g. a full-screen notice came up */

/* Startup screen: logo with a turning sweep, name, version, status. Fades out on hide. */
void sf_splash_show(const char *version);
void sf_splash_status(const char *text);
void sf_splash_hide(void);
bool sf_splash_visible(void);
bool sf_sighting_visible(void);
void sf_screens_set_sighting_cb(void (*tapped)(void));

/* Full-screen notice while the microSD card is open on a computer over USB. */
void sf_usb_show(bool show);
/* The USB notice's Done button: take the card back from the computer. */
void sf_screens_set_usb_done_cb(void (*cb)(void));
void sf_screens_set_long_press_cb(void (*cb)(void));
