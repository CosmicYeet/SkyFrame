#pragma once
#include <stdbool.h>
#include <stddef.h>
#define SF_WIFI_SSID_MAX 32
#define SF_WIFI_PASS_MAX 64
#define SF_WIFI_NETWORKS 5      /* saved Wi-Fi networks */
#define SF_DEFAULT_LAT 39.15    /* near BWI airport until the real center is set on the settings page */
#define SF_DEFAULT_LON (-76.70)
typedef struct {
    char ssid[SF_WIFI_SSID_MAX+1];
    char password[SF_WIFI_PASS_MAX+1];
} sf_wifi_network_t;
typedef struct {
    /* Saved networks, used slots first (no gaps); wifi[0] is the preferred one. */
    sf_wifi_network_t wifi[SF_WIFI_NETWORKS];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
    /* Screen off overnight, local hours 0-23; equal values = never off. */
    unsigned night_start_hour, night_end_hour;
    /* Sighting alerts: SF_ALERT_* bits, plus aircraft types ("B74*,A388"; '*' = any ending). */
    unsigned alert_flags;
    char alert_types[48];
    /* Seconds on screen per rotation for the verse of the day and Today's sky (0 = skip). */
    unsigned verse_seconds, today_seconds;
    /* Display options: SF_UI_* bits (0 = everything on). */
    unsigned ui_flags;
} sf_config_t;
#define SF_UI_NO_TRANSITIONS 1u   /* screens change instantly instead of animating */
#define SF_UI_SATELLITE 2u        /* the radar over a satellite picture of the area */
#define SF_UI_MASK (SF_UI_NO_TRANSITIONS|SF_UI_SATELLITE)
#define SF_DEFAULT_VERSE_SECONDS 20
#define SF_DEFAULT_TODAY_SECONDS 20
#define SF_ALERT_EMERGENCY 1u
#define SF_ALERT_VIP 2u
#define SF_ALERT_MILITARY 4u      /* military aircraft other than helicopters */
#define SF_ALERT_NO_OVERHEAD 8u   /* "Overhead now" cards off (stored inverted, so they start on) */
#define SF_ALERT_MASK (SF_ALERT_EMERGENCY|SF_ALERT_VIP|SF_ALERT_MILITARY|SF_ALERT_NO_OVERHEAD)
#define SF_DEFAULT_ALERTS (SF_ALERT_EMERGENCY|SF_ALERT_VIP|SF_ALERT_MILITARY)
#define SF_DEFAULT_ALERT_TYPES "B74*,A388"
/* Clean up a typed type list: upper case, comma separated, 2-4 letters/digits each (or a
   1-3 character start followed by '*'), at most 8. False (out untouched) if an entry is bad. */
bool sf_config_types_normalize(const char *in, char *out, size_t cap);
#define SF_DEFAULT_NIGHT_START 22
#define SF_DEFAULT_NIGHT_END 7
void sf_config_defaults(sf_config_t *cfg);
bool sf_config_valid(const sf_config_t *cfg);
/* Number of saved Wi-Fi networks (0 = never set up). */
int sf_config_network_count(const sf_config_t *cfg);
/* Slot of a saved network by name, or -1. */
int sf_config_find_network(const sf_config_t *cfg, const char *ssid);
/* Decode a form-urlencoded body; never log or echo the password. Transactional:
   bad input leaves the original config untouched.
   Wi-Fi keys: ssid (+ password) adds a network, or with a saved name changes its
   password (empty password keeps it); password alone changes the preferred network's
   password; remove=<name> forgets a network; prefer=<name> makes it the preferred one.
   On failure *why (if given) is a short message for the person, or NULL. */
bool sf_config_apply_form(sf_config_t *cfg, const char *body, size_t len, const char **why);
bool sf_config_parse_form(sf_config_t *cfg, const char *body, size_t len);

/* Saved-settings record (the NVS blob). Older record versions still decode, with new
   fields at their defaults, so a firmware update keeps a configured device's settings. */
#define SF_CONFIG_RECORD_VERSION 6
typedef struct { unsigned version; sf_config_t data; } sf_config_record_t;
bool sf_config_decode(const void *blob, size_t size, sf_config_t *out);
