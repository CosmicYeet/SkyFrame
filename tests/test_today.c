#include "skyframe_today.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static sf_plane_t plane(const char *icao, const char *cs, const char *type, int alt, float dist, float kt) {
    sf_plane_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.icao, sizeof(p.icao), "%s", icao);
    snprintf(p.callsign, sizeof(p.callsign), "%s", cs);
    snprintf(p.type_code, sizeof(p.type_code), "%s", type);
    snprintf(p.aircraft_type, sizeof(p.aircraft_type), "%s", type);
    snprintf(p.category, sizeof(p.category), "A3");
    snprintf(p.emergency, sizeof(p.emergency), "none");
    p.altitude_ft = alt; p.has_altitude = true;
    p.distance_miles = dist;
    p.speed_knots = kt; p.has_speed = true;
    return p;
}

int main(void) {
    /* Special traffic: emergency beats VIP beats military; lifeguard is not an emergency. */
    sf_plane_t p = plane("a1b2c3", "UAL1", "B738", 30000, 5, 450);
    assert(sf_plane_special(&p) == SF_SPECIAL_NONE);
    strcpy(p.squawk, "7700"); assert(sf_plane_special(&p) == SF_SPECIAL_EMERGENCY);
    strcpy(p.squawk, "1200"); strcpy(p.emergency, "lifeguard"); assert(sf_plane_special(&p) == SF_SPECIAL_NONE);
    strcpy(p.emergency, "minfuel"); assert(sf_plane_special(&p) == SF_SPECIAL_EMERGENCY);
    strcpy(p.emergency, "none"); strcpy(p.callsign, "AF1"); p.db_flags = 1; assert(sf_plane_special(&p) == SF_SPECIAL_VIP);
    strcpy(p.callsign, "RCH451"); assert(sf_plane_special(&p) == SF_SPECIAL_MILITARY);
    strcpy(p.callsign, "SAM44"); p.db_flags = 0; assert(sf_plane_special(&p) == SF_SPECIAL_VIP);

    /* Helicopters by category or type. */
    sf_plane_t h = plane("aaaaaa", "LIFE1", "EC35", 1500, 3, 120);
    assert(sf_is_helicopter(&h));
    strcpy(h.type_code, "ZZZZ"); strcpy(h.category, "A7"); assert(sf_is_helicopter(&h));
    strcpy(h.category, "A1"); assert(!sf_is_helicopter(&h));

    /* Type lists: exact codes, '*' prefixes, case and spacing don't matter. */
    assert(sf_type_list_match("B74*,A388", "B748") && sf_type_list_match("B74*,A388", "A388"));
    assert(!sf_type_list_match("B74*,A388", "B738") && !sf_type_list_match("B74*", "B7"));
    assert(sf_type_list_match(" c17 , b74* ", "C17") && !sf_type_list_match("", "B748"));
    assert(!sf_type_list_match("A38", "A388"));

    /* Rarity: an airliner is everyday, a 747 rarer, a VC-25 rarest. */
    sf_plane_t a = plane("111111", "DAL1", "B738", 30000, 5, 450), b = plane("222222", "CLX1", "B748", 30000, 5, 450);
    sf_plane_t c = plane("333333", "AF1", "VC25", 30000, 5, 450);
    assert(sf_type_rarity(&a) == 0 && sf_type_rarity(&b) == 6 && sf_type_rarity(&c) == 9);

    /* Today: each aircraft counted once; records follow airborne planes only. */
    static sf_today_t t;
    sf_today_reset(&t, 20261124);
    assert(sf_today_add(&t, &a, 20261124, 9));
    a.distance_miles = 2.5f;
    assert(sf_today_add(&t, &a, 20261124, 9) && t.planes == 1 && t.per_hour[9] == 1);   /* closer: a record, not a new plane */
    assert(t.closest.value == 2.5f && !strcmp(t.closest.who, "DAL1"));
    assert(!sf_today_add(&t, &a, 20261124, 10) && t.planes == 1);                        /* nothing new */
    assert(sf_today_add(&t, &b, 20261124, 10) && t.planes == 2 && t.per_hour[10] == 1);
    sf_plane_t g = plane("444444", "N123", "C172", 0, 0.5f, 5);
    g.on_ground = true;
    assert(sf_today_add(&t, &g, 20261124, 10) && t.planes == 3 && t.closest.value == 2.5f);   /* ground: not "closest" */
    sf_plane_t v = plane("555555", "TUG1", "", 0, 0.2f, 5);
    strcpy(v.category, "C1");
    assert(!sf_today_add(&t, &v, 20261124, 10) && t.planes == 3);                        /* vehicles don't count */
    assert(sf_today_add(&t, &c, 20261124, 11) && t.vip == 1 && t.notable_count == 1);
    const sf_today_type_t *r = sf_today_rarest(&t);
    assert(r && !strcmp(r->type_code, "VC25") && !strcmp(r->who, "AF1"));
    sf_plane_t junk = plane("666666", "BAD1", "B738", 90000, 9, 2000);                   /* broken report */
    assert(sf_today_add(&t, &junk, 20261124, 11) && t.highest.value == 30000 && t.fastest.value == 450);

    /* A new day starts over. */
    assert(sf_today_add(&t, &a, 20261125, 0) && t.planes == 1 && t.day == 20261125 && t.vip == 0);
    puts("PASS: special traffic, type lists, rarity, Today's sky tally");
    return 0;
}
