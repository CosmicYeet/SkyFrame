#pragma once
/* Special traffic and the "Today's sky" daily tally. Pure logic: no ESP-IDF, no LVGL. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "skyframe_engine.h"

/* What makes an aircraft worth an alert, most important last. */
typedef enum { SF_SPECIAL_NONE, SF_SPECIAL_MILITARY, SF_SPECIAL_VIP, SF_SPECIAL_EMERGENCY } sf_special_t;
/* Emergency: squawk 7500/7600/7700 or an emergency status (not "lifeguard", a medical flight).
   VIP: Air Force One/Two, a VC-25, SAM or EXEC flights. Military: adsb.fi's military flag. */
sf_special_t sf_plane_special(const sf_plane_t *p);
/* Helicopter by emitter category A7 or a well-known helicopter type. */
bool sf_is_helicopter(const sf_plane_t *p);
/* Comma-separated ICAO type list such as "B74*,A388": an entry ending in '*' matches any
   type starting with it. Case-insensitive. */
bool sf_type_list_match(const char *list, const char *type_code);
/* How unusual this aircraft is to see, 0 (everyday) to 9 (rare). */
int sf_type_rarity(const sf_plane_t *p);

/* ---- Today's sky ----------------------------------------------------------- */
#define SF_TODAY_MAX_SEEN 2048
#define SF_TODAY_MAX_TYPES 96
#define SF_TODAY_NOTABLE 4
typedef struct {
    char who[12];          /* callsign, else registration, else ICAO address; empty = none */
    char type_code[8];
    char desc[32];         /* adsb.fi description, e.g. "BOEING 747-8" */
    float value;           /* feet, miles or knots; for notables the sf_special_t */
} sf_today_pick_t;
typedef struct {
    char type_code[8], desc[32], who[12];   /* who: the first one seen */
    uint16_t count;
    int8_t score;          /* sf_type_rarity */
} sf_today_type_t;
typedef struct {
    int day;               /* local date as yyyymmdd; 0 = not started */
    unsigned planes;       /* different aircraft seen today */
    uint16_t per_hour[24]; /* new aircraft per local hour */
    unsigned military, vip, emergency;
    sf_today_pick_t highest, closest, fastest;   /* airborne only */
    sf_today_pick_t notable[SF_TODAY_NOTABLE];   /* VIP / emergency / military, most important kept */
    unsigned notable_count;
    sf_today_type_t types[SF_TODAY_MAX_TYPES];
    unsigned type_count;
    uint32_t seen[SF_TODAY_MAX_SEEN];
    unsigned seen_count;
} sf_today_t;
void sf_today_reset(sf_today_t *t, int day);
/* Count one sighting (call for every plane of every snapshot; repeats are recognized).
   A new day starts the tally over. Surface vehicles are ignored. True if anything changed. */
bool sf_today_add(sf_today_t *t, const sf_plane_t *p, int day, int hour);
/* The most unusual type seen today (highest rarity, then the fewest seen), or NULL. */
const sf_today_type_t *sf_today_rarest(const sf_today_t *t);
