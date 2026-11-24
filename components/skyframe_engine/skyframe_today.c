#include "skyframe_today.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool in_list(const char *t, const char *const *list, size_t n) {
    if (!t || !t[0]) return false;
    for (size_t i = 0; i < n; i++) if (!strcmp(t, list[i])) return true;
    return false;
}
#define IN(t, list) in_list(t, list, sizeof(list) / sizeof(list[0]))

sf_special_t sf_plane_special(const sf_plane_t *p) {
    if (!p) return SF_SPECIAL_NONE;
    if (!strcmp(p->squawk, "7500") || !strcmp(p->squawk, "7600") || !strcmp(p->squawk, "7700"))
        return SF_SPECIAL_EMERGENCY;
    if (p->emergency[0] && strcmp(p->emergency, "none") && strcmp(p->emergency, "lifeguard"))
        return SF_SPECIAL_EMERGENCY;
    if (!strcmp(p->callsign, "AF1") || !strcmp(p->callsign, "AF2") || !strcmp(p->type_code, "VC25") ||
        !strncmp(p->callsign, "SAM", 3) || !strncmp(p->callsign, "EXEC", 4))
        return SF_SPECIAL_VIP;
    if (p->db_flags & 1u) return SF_SPECIAL_MILITARY;
    return SF_SPECIAL_NONE;
}

bool sf_is_helicopter(const sf_plane_t *p) {
    static const char *const heli[] = {"EC20","EC25","EC30","EC35","EC45","EC55","EC75","H160","H125",
        "H135","H145","AS32","AS50","AS55","AS65","A109","A119","A139","A169","A189","AW39","B06",
        "B105","B407","B412","B429","B430","B505","R22","R44","R66","S61","S76","S92","H60","H47",
        "H64","H53","UH1","MI8","MI24","BK17","EN28","EN48","H500"};
    return p && ((p->category[0] == 'A' && p->category[1] == '7') || IN(p->type_code, heli));
}

bool sf_type_list_match(const char *list, const char *type) {
    if (!list || !type || !type[0]) return false;
    size_t tn = strlen(type);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        size_t n = (size_t)(e - p);
        if (n) {
            bool prefix = p[n - 1] == '*';
            size_t k = prefix ? n - 1 : n;
            if ((prefix ? tn >= k : tn == k) && !strncasecmp(p, type, k)) return true;
        }
        p = e;
    }
    return false;
}

int sf_type_rarity(const sf_plane_t *p) {
    static const char *const rare[] = {   /* seldom seen anywhere */
        "VC25","E4","E6","B52","B1","B2","U2","C5M","A124","A225","BLCF","A3ST","CONC","F22","F35",
        "V22","C32","C37A","C37B","C40","E3TF","E3CF","P8","K46","KC10","SR71","A400","AN22"};
    static const char *const quad[] = {   /* four engines */
        "B741","B742","B743","B744","B748","B74S","A388","A342","A343","A345","A346","C17","IL76",
        "IL96","K35R","K35E","C135","E3TF"};
    static const char *const wide[] = {   /* wide-bodies */
        "B762","B763","B764","B772","B773","B77L","B77W","B778","B779","B788","B789","B78X","A332",
        "A333","A338","A339","A359","A35K","A306","A30B","A310","MD11","DC10"};
    if (!p) return 0;
    int s = 0;
    #define AT_LEAST(v) do { if (s < (v)) s = (v); } while (0)
    if (IN(p->type_code, rare)) AT_LEAST(9);
    if (sf_plane_special(p) == SF_SPECIAL_VIP) AT_LEAST(9);
    if (p->db_flags & 1u) AT_LEAST(7);
    if (p->category[0] == 'A' && p->category[1] == '6') AT_LEAST(6);   /* high performance */
    if (IN(p->type_code, quad)) AT_LEAST(6);
    if (p->category[0] == 'B') AT_LEAST(5);   /* glider, balloon, ultralight, drone */
    if (p->db_flags & 2u) AT_LEAST(4);        /* adsb.fi "interesting" */
    if (IN(p->type_code, wide)) AT_LEAST(3);
    if (sf_is_helicopter(p)) AT_LEAST(3);
    if (p->category[0] == 'A' && p->category[1] == '1') AT_LEAST(1);
    #undef AT_LEAST
    return s;
}

/* ---- Today's sky ----------------------------------------------------------- */
void sf_today_reset(sf_today_t *t, int day) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->day = day;
}

/* ICAO address as a number; non-ICAO ("~" prefixed) addresses get the top bit. */
static bool address_key(const char *icao, uint32_t *key) {
    bool other = icao[0] == '~';
    const char *h = other ? icao + 1 : icao;
    char *end;
    if (!h[0]) return false;
    unsigned long v = strtoul(h, &end, 16);
    if (*end || v > 0xFFFFFFul) return false;
    *key = (uint32_t)v | (other ? 0x80000000u : 0);
    return true;
}
static void pick(sf_today_pick_t *dst, const sf_plane_t *p, const char *who, float value) {
    snprintf(dst->who, sizeof(dst->who), "%s", who);
    snprintf(dst->type_code, sizeof(dst->type_code), "%s", p->type_code);
    snprintf(dst->desc, sizeof(dst->desc), "%s", p->aircraft_type);
    dst->value = value;
}
static void count_type(sf_today_t *t, const sf_plane_t *p, const char *who) {
    if (!p->type_code[0]) return;
    for (unsigned i = 0; i < t->type_count; i++)
        if (!strcmp(t->types[i].type_code, p->type_code)) {
            if (t->types[i].count < UINT16_MAX) t->types[i].count++;
            if (!t->types[i].desc[0]) snprintf(t->types[i].desc, sizeof(t->types[i].desc), "%s", p->aircraft_type);
            return;
        }
    if (t->type_count >= SF_TODAY_MAX_TYPES) return;
    sf_today_type_t *e = &t->types[t->type_count++];
    snprintf(e->type_code, sizeof(e->type_code), "%s", p->type_code);
    snprintf(e->desc, sizeof(e->desc), "%s", p->aircraft_type);
    snprintf(e->who, sizeof(e->who), "%s", who);
    e->count = 1;
    e->score = (int8_t)sf_type_rarity(p);
}
static void add_notable(sf_today_t *t, const sf_plane_t *p, const char *who, sf_special_t kind) {
    if (t->notable_count < SF_TODAY_NOTABLE) { pick(&t->notable[t->notable_count++], p, who, (float)kind); return; }
    /* Full: a more important one replaces the least important (the latest of those). */
    unsigned low = 0;
    for (unsigned i = 1; i < t->notable_count; i++) if (t->notable[i].value <= t->notable[low].value) low = i;
    if ((float)kind > t->notable[low].value) pick(&t->notable[low], p, who, (float)kind);
}

bool sf_today_add(sf_today_t *t, const sf_plane_t *p, int day, int hour) {
    if (!t || !p || p->category[0] == 'C') return false;   /* C*: surface vehicles and obstacles */
    if (day != t->day) sf_today_reset(t, day);
    uint32_t key;
    if (!address_key(p->icao, &key)) return false;
    const char *who = p->callsign[0] ? p->callsign : p->registration[0] ? p->registration : p->icao;
    bool changed = false, known = false;
    for (unsigned i = 0; i < t->seen_count; i++) if (t->seen[i] == key) { known = true; break; }
    /* A full table can't tell new from seen: count no more new planes today rather than
       recount the same ones on every update. Records below still update. */
    if (!known && t->seen_count >= SF_TODAY_MAX_SEEN) known = true;
    if (!known) {
        if (t->seen_count < SF_TODAY_MAX_SEEN) t->seen[t->seen_count++] = key;
        t->planes++;
        if (hour >= 0 && hour < 24 && t->per_hour[hour] < UINT16_MAX) t->per_hour[hour]++;
        count_type(t, p, who);
        sf_special_t kind = sf_plane_special(p);
        if (kind == SF_SPECIAL_MILITARY) t->military++;
        else if (kind == SF_SPECIAL_VIP) t->vip++;
        else if (kind == SF_SPECIAL_EMERGENCY) t->emergency++;
        if (kind != SF_SPECIAL_NONE) add_notable(t, p, who, kind);
        changed = true;
    }
    if (!p->on_ground) {
        /* Ignore obviously broken reports (over 60,000 ft, faster than 1,000 kt). */
        if (p->has_altitude && p->altitude_ft > 0 && p->altitude_ft <= 60000 &&
            (!t->highest.who[0] || (float)p->altitude_ft > t->highest.value)) {
            pick(&t->highest, p, who, (float)p->altitude_ft); changed = true;
        }
        if (!t->closest.who[0] || p->distance_miles < t->closest.value) {
            pick(&t->closest, p, who, p->distance_miles); changed = true;
        }
        if (p->has_speed && p->speed_knots <= 1000.f && (!t->fastest.who[0] || p->speed_knots > t->fastest.value)) {
            pick(&t->fastest, p, who, p->speed_knots); changed = true;
        }
    }
    return changed;
}

const sf_today_type_t *sf_today_rarest(const sf_today_t *t) {
    const sf_today_type_t *best = NULL;
    for (unsigned i = 0; t && i < t->type_count; i++) {
        const sf_today_type_t *e = &t->types[i];
        if (!best || e->score > best->score || (e->score == best->score && e->count < best->count)) best = e;
    }
    return best;
}
