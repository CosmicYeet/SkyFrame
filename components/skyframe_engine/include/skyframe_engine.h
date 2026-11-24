#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_MAX_PLANES 12
#define SF_DISPLAY_MILES 10.0
#define SF_ACTIVATION_MILES 5.0
#define SF_ADSB_MAX_BYTES (512u * 1024u)   /* largest adsb.fi response accepted */

typedef struct {
    char callsign[12];
    char aircraft_type[32];
    char icao[9];
    char registration[12];   /* "r", e.g. N69063 */
    char type_code[8];       /* "t", ICAO type designator, e.g. B764 */
    char operator_name[48];  /* "ownOp" from the registry */
    char year[6];            /* build year */
    char squawk[6];
    char category[4];        /* ADS-B emitter category, e.g. A3 */
    char emergency[12];      /* "none", "general", "lifeguard", "minfuel", "nordo", "unlawful", ... */
    unsigned db_flags;       /* adsb.fi dbFlags: 1 military, 2 interesting, 4 PIA, 8 LADD */
    int vert_rate_fpm;
    bool has_vert_rate;
    double latitude;
    double longitude;
    float position_age_s;    /* "seen_pos": how old the position already was when served */
    float bearing_deg;       /* clockwise from true north, relative to home */
    float distance_miles;
    float heading_deg;       /* true ground track if present; heading fallback */
    float speed_knots;
    int altitude_ft;
    bool on_ground;
    bool has_heading;
    bool has_speed;
    bool has_altitude;
} sf_plane_t;

typedef struct {
    sf_plane_t planes[SF_MAX_PLANES];
    size_t count;
    bool api_ok;             /* successful valid JSON, even if zero targets */
    size_t ignored;
    size_t airport_ground;   /* aircraft on the ground at BWI (within 2.5 miles of it) */
} sf_aircraft_result_t;

/* Parse a valid adsb.fi /v2/lat/{lat}/lon/{lon}/dist/{nm} JSON response.
   Returns false for malformed/unrecognized payload, never silently treats it as clear sky.
   Inputs: valid lat/lon degrees; JSON may contain up to SF_ADSB_MAX_BYTES of data.
   Filters locations not reported recently (seen_pos > 35 seconds).
   Chooses SF_MAX_PLANES within display radius: airborne first (nearest first), then the
   nearest on the ground with whatever slots are left.
   No heap allocation; synchronous parsing; invoke from a network task, not the LVGL draw callback. */
bool sf_parse_adsb(const char *json, size_t length, double home_lat, double home_lon,
                   double display_miles, sf_aircraft_result_t *out);
void sf_geo(double home_lat, double home_lon, double lat, double lon,
            float *distance_miles, float *bearing_deg);
/* An airborne aircraft within activation_miles (parked and taxiing ones don't count). */
bool sf_has_active_traffic(const sf_aircraft_result_t *r, float activation_miles);

/* Rotation: radar -> weather -> today's sky -> verse -> photos -> birthday -> radar.
   Today's sky, verse and birthday are optional: a duration of 0 skips them. */
typedef enum { SF_SCREEN_RADAR, SF_SCREEN_WEATHER, SF_SCREEN_PHOTOS, SF_SCREEN_VERSE, SF_SCREEN_BIRTHDAY,
               SF_SCREEN_TODAY, SF_SCREEN_COUNT } sf_screen_t;
typedef struct {
    sf_screen_t screen;
    uint32_t phase_start_ms;
    uint32_t quiet_since_ms;
    bool quiet_timing;
    bool feed_valid;
    uint32_t radar_ms, weather_ms, photos_ms;
    uint32_t verse_ms, birthday_ms;   /* 0 = not in the rotation */
    uint32_t today_ms;                /* 0 = not in the rotation */
} sf_scheduler_t;
/* Screen that follows `current` in the rotation. */
sf_screen_t sf_schedule_next(const sf_scheduler_t *s, sf_screen_t current);
/* Move the rotation on to the next screen now (a long press); it then runs its normal time. */
void sf_schedule_skip(sf_scheduler_t *s, uint32_t now_ms);
/* Include or skip the optional screens (seconds; 0 = skip). */
void sf_schedule_set_optional(sf_scheduler_t *s, unsigned verse_sec, unsigned birthday_sec);
void sf_schedule_set_today(sf_scheduler_t *s, unsigned seconds);
/* Show this screen now (e.g. the radar after tapping a sighting alert); it then runs its normal time. */
void sf_schedule_jump(sf_scheduler_t *s, sf_screen_t screen, uint32_t now_ms);
/* Keep the current screen up (call every tick while it should stay): restarts its time. */
void sf_schedule_hold(sf_scheduler_t *s, uint32_t now_ms);

void sf_schedule_init(sf_scheduler_t *s, uint32_t now_ms);
/* Call once per UI tick; feed_ok=false freezes traffic-dependent transitions.
   While weather/photos cycle, traffic does NOT interrupt. On next radar phase it resumes.
   details_open=true pauses automatic transitions by advancing phase timestamps. */
sf_screen_t sf_schedule_tick(sf_scheduler_t *s, uint32_t now_ms,
                             bool feed_ok, bool aircraft_within_activation,
                             bool details_open);

/* The display fonts are ASCII only. Fold UTF-8 text to ASCII: accented Latin letters to
   their base letter ("Zurich", "Sao Paulo"), curly quotes/dashes/ellipsis to plain ones;
   anything else is dropped. Output is never longer than input (in == out is allowed). */
void sf_ascii_fold(const char *in, char *out, size_t cap);
/* ASCII stand-in for a Latin-1 code point (U+00C0..U+00FF), or 0 if none. */
char sf_latin1_base(unsigned code);

/* Family birthdays, one per line of /birthdays.txt on the microSD card:
     Name, YYYY-MM-DD [, photo.jpg] [, owner]
   The year may be left off ("MM-DD") when unknown; then no age is shown.
   "owner" marks the person the device belongs to (special birthday screen).
   Blank lines and lines starting with '#' are ignored. */
typedef struct {
    char name[32];
    int year;               /* 0 = unknown */
    int month, day;
    char photo[48];         /* file name in /birthdays, may be empty */
    bool owner;
} sf_birthday_t;
bool sf_birthday_parse_line(const char *line, sf_birthday_t *out);
/* "message: With love from your family" -> the owner's birthday message. True if the
   line is a message line (out then holds the text, folded to ASCII). */
bool sf_birthday_parse_message(const char *line, char *out, size_t cap);
/* Feb 29 birthdays are celebrated on Feb 28 in non-leap years. */
bool sf_birthday_is_today(const sf_birthday_t *b, int year, int month, int day);
/* Age reached this year, or -1 when the birth year is unknown. */
int sf_birthday_age(const sf_birthday_t *b, int year);
/* "st", "nd", "rd" or "th" for 1st, 2nd, 3rd, 57th, 11th, 112th... */
const char *sf_ordinal_suffix(int n);

/* Looked-up details for one aircraft (adsbdb.com): airframe and, when the callsign
   is known, the airline and route. Empty strings mean "not available". */
typedef struct {
    char code[5], name[48], city[32];
} sf_airport_t;
typedef struct {
    char icao[9];
    bool lookup_ok;          /* false: network/server error, so "not found" is unknown */
    bool aircraft_found, route_found;
    char manufacturer[32], model[40], type_code[8], registration[12];
    char owner[48], owner_country[32];
    char airline[40];
    sf_airport_t origin, destination;
    char photo_url[160];
} sf_aircraft_info_t;

/* Fixed landmark coordinates: BWI airport reference point (not flight tracking). */
#define SF_BWI_LATITUDE 39.1797894
#define SF_BWI_LONGITUDE (-76.6696376)

void sf_schedule_set_durations(sf_scheduler_t *s,unsigned radar_sec,unsigned weather_sec,unsigned photos_sec);
