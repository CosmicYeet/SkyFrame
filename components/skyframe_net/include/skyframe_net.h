#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "skyframe_engine.h"

/* ~1 KB per aircraft in adsb.fi responses; a 100-mile radius near a hub can exceed 400. */
#define SF_NET_AIRCRAFT_LIMIT (512 * 1024)
#define SF_NET_WEATHER_LIMIT (24 * 1024)
#define SF_WEATHER_HOURS 4

typedef struct {
    float temp_f;
    float feels_f;           /* NAN when unavailable */
    float high_f;
    float low_f;
    int weather_code;
    bool is_day;
    int humidity;            /* %, -1 when unavailable */
    float wind_mph;          /* NAN when unavailable */
    float wind_dir_deg;      /* direction the wind comes FROM; NAN when unavailable */
    int precip_chance;       /* today's max %, -1 when unavailable */
    char sunrise[6], sunset[6];   /* local "HH:MM", empty when unavailable */
    char observation_time[40];
    int utc_offset_s;        /* local time = UTC + this (follows daylight saving) */
    bool has_utc_offset;
    bool valid;
    struct { char time[12]; float temp_f; int weather_code; int precip_chance; bool is_day; bool valid; } hourly[SF_WEATHER_HOURS];
} sf_weather_t;

/* All network calls are BLOCKING; call in a worker task after Wi-Fi is connected.
   Never run these while holding the LVGL/UI mutex. */
esp_err_t sf_net_get_aircraft(double lat, double lon, float radius_miles,
                             sf_aircraft_result_t *out);
esp_err_t sf_net_get_weather(double lat, double lon, sf_weather_t *out);

/* Airframe + route details from adsbdb.com for a 6-hex-digit ICAO address.
   ESP_OK with aircraft_found=false means the database does not know this aircraft. */
/* The airframe (adsbdb.com) and, from its callsign, the route: adsb.lol's route files first
   (the leg through `home` for a multi-stop flight, see sf_parse_route; climb > 0 climbing,
   < 0 descending), adsbdb's route when they have none. */
esp_err_t sf_net_get_aircraft_info(const char *icao, const char *callsign, const char *home, int climb,
                                   sf_aircraft_info_t *info);

/* BibleGateway's verse of the day in the NKJV, or the ESV if the NKJV request fails;
   never any other translation. Text is plain ASCII for the display fonts. */
typedef struct {
    char text[640];
    char reference[48];     /* "Psalm 95:1-2" */
    char version[8];        /* "NKJV" or "ESV" */
    char credit[48];        /* "Powered by BibleGateway.com" (required credit) */
    bool valid;
} sf_verse_t;
/* HTML entities (&ldquo; &#8217; &amp; ...) to plain ASCII. */
void sf_verse_decode_entities(const char *in, char *out, size_t cap);
esp_err_t sf_net_get_verse(sf_verse_t *out);

/* Active National Weather Service alerts for a point (US only), most serious first. */
#define SF_MAX_WX_ALERTS 3
#define SF_NET_ALERTS_LIMIT (256 * 1024)
typedef struct {
    char event[48];          /* "Severe Thunderstorm Warning" */
    char area[96];           /* "Anne Arundel; Howard" */
    char text[720];          /* description, then instructions; paragraphs end in '
' */
    uint8_t level;           /* 3 warning, 2 watch, 1 advisory, 0 statement or other */
    int64_t ends_utc;        /* Unix time it ends (0: not given) */
    int ends_date;           /* local yyyymmdd of the end, at the alert's own UTC offset */
    int ends_minute;         /* local minutes after midnight; -1: not given */
} sf_wx_alert_t;
typedef struct {
    sf_wx_alert_t alert[SF_MAX_WX_ALERTS];
    int count;               /* kept here */
    int total;               /* active for the point (may be more than count) */
} sf_wx_alerts_t;
esp_err_t sf_net_get_wx_alerts(double lat, double lon, sf_wx_alerts_t *out);
/* "2026-11-23T18:45:00-05:00" -> Unix time, plus the local date and minute it names. */
bool sf_parse_iso_time(const char *s, int64_t *utc, int *local_date, int *local_minute);

/* Exclusive use of the network for a long transfer (the firmware updater): only one TLS
   session runs at a time, because internal RAM is short. Other fetches wait meanwhile. */
bool sf_net_lock(uint32_t wait_ms);
void sf_net_unlock(void);

/* Download an HTTPS resource into a PSRAM buffer the caller frees. */
esp_err_t sf_net_get_bytes(const char *url, size_t max_bytes, uint8_t **data, size_t *len);
