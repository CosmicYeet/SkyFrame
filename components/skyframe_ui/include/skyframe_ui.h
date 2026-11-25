#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "skyframe_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SKYFRAME_MAX_AIRCRAFT 12
/* Aircraft photo frame in the details view (RGB565, row-major). */
#define SKYFRAME_PHOTO_W 280
#define SKYFRAME_PHOTO_H 156

typedef struct {
    char callsign[12];
    char aircraft_type[32];  /* ADS-B description, e.g. BOEING 767-400 */
    char icao[9];
    char registration[12];
    char type_code[8];
    char operator_name[48];
    char year[6];
    char squawk[6];
    char category[4];
    char emergency[12];
    unsigned db_flags;
    char origin[5];
    char destination[5];
    float bearing_deg;     /* Bearing FROM radar center, clockwise from north */
    float distance_miles;
    float heading_deg;     /* True track, clockwise from north */
    int altitude_ft;
    int vert_rate_fpm;
    float speed_knots;
    bool on_ground;
    bool has_vert_rate;
} skyframe_aircraft_t;

/* Call after Waveshare's display/LVGL initialization, while holding its LVGL lock. */
void skyframe_ui_init(lv_obj_t *parent);

/* Called only from the LVGL/UI task, or while the board's LVGL lock is held. */
void skyframe_ui_set_aircraft(const skyframe_aircraft_t *aircraft, uint8_t count);

/* Call on LVGL thread; distance/bearing calculated via sf_geo() using configured center.
   Airport is purely a landmark and never counts as active traffic. */
void skyframe_ui_set_airport(float distance_miles, float bearing_deg, bool visible);
/* Aircraft on the ground at the airport, shown as a number next to its name (0: just the name). */
void skyframe_ui_set_airport_ground(unsigned count);

#ifdef __cplusplus
}
#endif

/* True while an aircraft detail view is visible. LVGL thread only. */
bool skyframe_ui_details_open(void);
void skyframe_ui_close_details(void);
/* Radar status line; text must be a string literal (stored by pointer). NULL hides it. */
void skyframe_ui_set_status(const char *text);
void skyframe_ui_show(bool show);

/* Tapping a plane reports it (ICAO address, callsign, type...) so details can be looked up. */
void skyframe_ui_set_select_cb(void (*cb)(const skyframe_aircraft_t *aircraft));
/* Looked-up airframe/route details; ignored unless that aircraft's details are open. */
void skyframe_ui_update_info(const sf_aircraft_info_t *info);
/* Hand over a SKYFRAME_PHOTO_W x SKYFRAME_PHOTO_H RGB565 photo. Returns true when the UI
   took ownership (it frees it later); false means the caller must free it. */
bool skyframe_ui_set_photo(const char *icao, uint16_t *pixels);
/* No photo exists (or it failed to load) for this aircraft. */
void skyframe_ui_photo_unavailable(const char *icao);
void skyframe_ui_set_radius(float miles);

/* Radar clock ("10:45 PM"; empty hides it). */
void skyframe_ui_set_clock(const char *text);
/* Counter of panel refreshes; paces the radar sweep to real frames (NULL: millisecond clock). */
void skyframe_ui_set_frame_clock(uint32_t (*clock)(void));
/* Wi-Fi signal icon, top left of the scope: 0-4 bars, or -1 when not connected. */
void skyframe_ui_set_wifi(int bars);
/* Radar center coordinates, shown at the bottom of the scope. */
void skyframe_ui_set_location(double latitude, double longitude);
/* Satellite view: the scope over a picture of the area (480x480 RGB565, PSRAM, centered on
   the radar and scaled to it). Takes ownership of `pixels` (NULL drops it). */
void skyframe_ui_set_satellite_image(uint16_t *pixels);
bool skyframe_ui_has_satellite_image(void);
void skyframe_ui_show_satellite(bool on);
