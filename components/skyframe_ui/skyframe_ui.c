#include "skyframe_ui.h"
#include "skyframe_screens.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"

/* LVGL 8.x application. Hardware initialization stays in Waveshare's demo. */
#define SIZE 480
#define CX 240
#define CY 240
#define RADAR_R 204
#define PI_F 3.14159265f
#define GREEN lv_color_hex(0x7CF5A3)
#define DIM_GREEN lv_color_hex(0x276B46)   /* rings and crosshair */
#define DIM_TEXT lv_color_hex(0x5AA77A)    /* small captions: readable on BG */
#define BG lv_color_hex(0x06150D)

static skyframe_aircraft_t traffic[SKYFRAME_MAX_AIRCRAFT];
static uint8_t traffic_count;
static lv_obj_t *root, *detail_screen;
/* One transparent 42x42 object per plane: it is the tap target and draws the plane's
   glyph itself, so a moving plane invalidates just its old and new box. */
#define PLANE_BOX 42
static lv_obj_t *planes[SKYFRAME_MAX_AIRCRAFT];
static lv_obj_t *plane_labels[SKYFRAME_MAX_AIRCRAFT];
#define PLANE_LABEL_FONT (&lv_font_montserrat_12)
#define PLANE_LABEL_W 82            /* fits "Near ground" and an 11-letter callsign */
#define PLANE_LABEL_H 30          /* two lines of the 12 px font */
#define VISIBLE_R 234             /* keep labels inside the round glass */
static lv_obj_t *status_label;
static lv_obj_t *airport_pin, *airport_text;
static int airport_x = -1, airport_y;     /* pin center on screen; -1 when not shown */
static unsigned airport_ground;
#define AIRPORT_PIN 17                     /* marker size, px */
#define GROUND_DOT 0x4C8C66                /* dim green: parked and taxiing aircraft */
static lv_obj_t *sweep;
static lv_timer_t *sweep_timer;
static lv_point_t sweep_tip = {240, 36};
static char selected_icao[9];
static float display_radius=10.f;
static void (*on_selected)(const skyframe_aircraft_t *aircraft);
static lv_obj_t *clock_label, *coords_lat, *coords_lon, *range_label;
#define WIFI_BARS 4
static lv_obj_t *wifi_icon, *wifi_bar[WIFI_BARS];

static lv_obj_t *new_label(lv_obj_t *parent, const char *text, int x, int y,
                           lv_color_t color, const lv_font_t *font) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_color(o, color, 0);
    if (font) lv_obj_set_style_text_font(o, font, 0);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    return o;
}

static void point_from_bearing(float distance, float bearing, int *x, int *y) {
    const float radians = bearing * PI_F / 180.f;
    *x = CX + (int)lroundf(sinf(radians) * distance * (RADAR_R / display_radius));
    *y = CY - (int)lroundf(cosf(radians) * distance * (RADAR_R / display_radius));
}

/* "8,500 ft" style altitude, GROUND, or "Near ground" for a moving plane whose
   pressure altitude reads below zero (normal close to the runway on high-pressure days). */
static void format_altitude(const skyframe_aircraft_t *a, char *buf, size_t cap) {
    if (a->on_ground) { snprintf(buf, cap, "GROUND"); return; }
    int ft = a->altitude_ft;
    if (ft < 0) snprintf(buf, cap, "Near ground");
    else if (ft >= 1000) snprintf(buf, cap, "%d,%03d ft", ft / 1000, ft % 1000);
    else snprintf(buf, cap, "%d ft", ft);
}

/* Traffic worth pointing out, most important first. Colors the radar plane and label;
   the details page shows the badge text. */
typedef enum { KIND_NORMAL, KIND_NOTABLE, KIND_MILITARY, KIND_VIP, KIND_EMERGENCY } traffic_kind_t;
static const uint32_t kind_color[] = {0x7CF5A3, 0x6EDCFF, 0xFFC857, 0xC89BFF, 0xFF5A5A};
/* Short badge for the photo, plus a plain-language reason for the details page. */
static traffic_kind_t classify_why(const skyframe_aircraft_t *a, char *badge, size_t cap,
                                   char *why, size_t why_cap) {
    if (badge && cap) badge[0] = 0;
    if (why && why_cap) why[0] = 0;
    #define BADGE(...) do { if (badge) snprintf(badge, cap, __VA_ARGS__); } while (0)
    #define WHY(...) do { if (why) snprintf(why, why_cap, __VA_ARGS__); } while (0)
    if (!strcmp(a->squawk, "7700")) {
        BADGE("EMERGENCY  7700");
        WHY("Squawking 7700: the crew has declared an emergency. Air traffic control gives them priority.");
        return KIND_EMERGENCY;
    }
    if (!strcmp(a->squawk, "7600")) {
        BADGE("RADIO FAILURE  7600");
        WHY("Squawking 7600: radio failure. The crew can't talk to air traffic control and follows set procedures.");
        return KIND_EMERGENCY;
    }
    if (!strcmp(a->squawk, "7500")) {
        BADGE("HIJACK CODE  7500");
        WHY("Squawking 7500, the code for unlawful interference (hijacking). It is sometimes set by mistake.");
        return KIND_EMERGENCY;
    }
    if (a->emergency[0] && strcmp(a->emergency, "none")) {
        if (!strcmp(a->emergency, "lifeguard")) {
            BADGE("MEDICAL FLIGHT");
            WHY("Lifeguard flight: carrying a patient or medical team, with priority from air traffic control.");
            return KIND_NOTABLE;
        }
        static const struct { const char *code, *text; } em[] = {
            {"minfuel", "Low on fuel: the crew has told air traffic control they need priority to land."},
            {"nordo", "No radio: the aircraft can't communicate with air traffic control."},
            {"unlawful", "Unlawful interference reported (hijacking)."},
            {"downed", "Downed aircraft reported."},
            {"general", "The crew has declared a general emergency."}};
        BADGE("EMERGENCY  %.12s", a->emergency);
        WHY("The aircraft is broadcasting an emergency status (%.12s).", a->emergency);
        for (size_t k = 0; k < sizeof(em) / sizeof(em[0]); k++)
            if (!strcmp(a->emergency, em[k].code)) WHY("%s", em[k].text);
        return KIND_EMERGENCY;
    }
    /* Presidential and government VIP flights. */
    if (!strcmp(a->callsign, "AF1") || !strcmp(a->callsign, "AF2")) {
        bool one = a->callsign[2] == '1';
        BADGE(one ? "AIR FORCE ONE" : "AIR FORCE TWO");
        WHY(one ? "Air Force One: the call sign used by any Air Force aircraft carrying the President."
                : "Air Force Two: the call sign used by any Air Force aircraft carrying the Vice President.");
        return KIND_VIP;
    }
    if (!strcmp(a->type_code, "VC25")) {
        BADGE("GOVERNMENT VIP");
        WHY("VC-25: one of the two presidential Boeing 747s, which fly as Air Force One with the President aboard.");
        return KIND_VIP;
    }
    if (!strncmp(a->callsign, "SAM", 3) || !strncmp(a->callsign, "EXEC", 4)) {
        BADGE("GOVERNMENT VIP");
        WHY(!strncmp(a->callsign, "SAM", 3)
            ? "Special Air Mission: an Air Force flight carrying senior government officials."
            : "Executive flight: carrying members of the President's family or senior officials.");
        return KIND_VIP;
    }
    if (a->db_flags & 1u) {
        BADGE("MILITARY");
        WHY("Listed as a military aircraft in the adsb.fi aircraft database.");
        return KIND_MILITARY;
    }
    if (a->db_flags & 2u) {
        BADGE("NOTABLE AIRCRAFT");
        WHY("Flagged as interesting in the adsb.fi database: for example a rare, historic or government aircraft.");
        return KIND_NOTABLE;
    }
    if (!strcmp(a->category, "A7")) { BADGE("HELICOPTER"); return KIND_NORMAL; }
    #undef BADGE
    #undef WHY
    return KIND_NORMAL;
}
static traffic_kind_t classify(const skyframe_aircraft_t *a, char *badge, size_t cap) {
    return classify_why(a, badge, cap, NULL, 0);
}

typedef struct { int x1, y1, x2, y2; } box_t;
static bool box_inside_circle(const box_t *b) {
    const int xs[2] = {b->x1, b->x2}, ys[2] = {b->y1, b->y2};
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
        int dx = xs[i] - CX, dy = ys[j] - CY;
        if (dx*dx + dy*dy > VISIBLE_R*VISIBLE_R) return false;
    }
    return true;
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_point_t *pts, lv_color_t color, int width);

/* ---- Aircraft glyphs -----------------------------------------------------
   Small vector shapes, nose up (north) before rotation, about 24 px across at scale 1.
   The radar draws them at scale 1; the details page draws a large one as the photo
   placeholder and a small one on the route line. */
typedef enum { GLYPH_JET, GLYPH_WIDEBODY, GLYPH_JUMBO, GLYPH_BIZJET, GLYPH_TURBOPROP, GLYPH_LIGHT,
               GLYPH_HELI, GLYPH_TILTROTOR, GLYPH_FIGHTER, GLYPH_GLIDER,   /* rotate with the track */
               GLYPH_BALLOON, GLYPH_DRONE, GLYPH_GROUND } glyph_t;          /* always upright */
typedef struct { int8_t f1, s1, f2, s2; uint8_t w; } stroke_t;   /* forward/side units */
static const stroke_t jet_strokes[] = {          /* narrow-body: fuselage, swept wings, tailplane */
    {-11,0,12,0,3}, {2,0,-5,-12,2}, {2,0,-5,12,2}, {-9,0,-13,-5,2}, {-9,0,-13,5,2}};
static const stroke_t widebody_strokes[] = {     /* wide fuselage, long swept wings, two engines */
    {-12,0,13,0,4}, {2,0,-6,-14,3}, {2,0,-6,14,3}, {3,-6,0,-6,3}, {3,6,0,6,3},
    {-10,0,-14,-6,2}, {-10,0,-14,6,2}};
static const stroke_t jumbo_strokes[] = {        /* long fuselage, big swept wings, four engines */
    {-13,0,14,0,4}, {3,0,-7,-15,3}, {3,0,-7,15,3}, {2,-5,-1,-5,3}, {2,5,-1,5,3},
    {-1,-10,-4,-10,3}, {-1,10,-4,10,3}, {-11,0,-15,-6,2}, {-11,0,-15,6,2}};
static const stroke_t bizjet_strokes[] = {       /* slim body, small swept wings, engines at the tail, T-tail */
    {-11,0,11,0,3}, {1,0,-5,-10,2}, {1,0,-5,10,2}, {-5,-3,-9,-3,3}, {-5,3,-9,3,3},
    {-11,0,-13,-5,2}, {-11,0,-13,5,2}};
static const stroke_t turboprop_strokes[] = {    /* straight wing, two engine pods with propellers */
    {-11,0,10,0,3}, {2,-13,2,13,2}, {5,-5,0,-5,3}, {5,5,0,5,3},
    {6,-3,6,-7,2}, {6,3,6,7,2}, {-10,-4,-10,4,2}};
static const stroke_t tiltrotor_strokes[] = {    /* short wing with a big rotor on each tip */
    {-10,0,9,0,3}, {1,-9,1,9,2}, {6,-9,-4,-9,2}, {6,9,-4,9,2}, {1,-14,1,-4,2}, {1,14,1,4,2},
    {-10,-3,-10,3,2}};
static const stroke_t fighter_strokes[] = {      /* pointed nose, delta wing, twin tails */
    {-10,0,13,0,3}, {5,-1,-6,-10,2}, {5,1,-6,10,2}, {-6,-10,-6,-3,2}, {-6,10,-6,3,2},
    {-6,-3,-11,-4,2}, {-6,3,-11,4,2}};
static const stroke_t light_strokes[] = {        /* fuselage, straight wing, tail, propeller */
    {-10,0,9,0,3}, {3,-11,3,11,2}, {-9,-4,-9,4,2}, {11,-3,11,3,2}};
static const stroke_t glider_strokes[] = {       /* slim body, very long wings, T-tail */
    {-11,0,8,0,2}, {2,-14,2,14,2}, {-11,-3,-11,3,2}};
static const stroke_t heli_strokes[] = {         /* cabin, tail boom, tail rotor, main rotor X */
    {0,0,5,0,7}, {-1,0,-12,0,2}, {-12,-3,-12,3,2}, {9,-7,-5,7,2}, {9,7,-5,-7,2}};
static const struct { const stroke_t *s; uint8_t n; } glyph_strokes[] = {
    [GLYPH_JET] = {jet_strokes, sizeof(jet_strokes) / sizeof(stroke_t)},
    [GLYPH_WIDEBODY] = {widebody_strokes, sizeof(widebody_strokes) / sizeof(stroke_t)},
    [GLYPH_JUMBO] = {jumbo_strokes, sizeof(jumbo_strokes) / sizeof(stroke_t)},
    [GLYPH_BIZJET] = {bizjet_strokes, sizeof(bizjet_strokes) / sizeof(stroke_t)},
    [GLYPH_TURBOPROP] = {turboprop_strokes, sizeof(turboprop_strokes) / sizeof(stroke_t)},
    [GLYPH_TILTROTOR] = {tiltrotor_strokes, sizeof(tiltrotor_strokes) / sizeof(stroke_t)},
    [GLYPH_FIGHTER] = {fighter_strokes, sizeof(fighter_strokes) / sizeof(stroke_t)},
    [GLYPH_LIGHT] = {light_strokes, sizeof(light_strokes) / sizeof(stroke_t)},
    [GLYPH_HELI] = {heli_strokes, sizeof(heli_strokes) / sizeof(stroke_t)},
    [GLYPH_GLIDER] = {glider_strokes, sizeof(glider_strokes) / sizeof(stroke_t)}};
static bool glyph_rotates(glyph_t g) { return g <= GLYPH_GLIDER; }

/* ICAO type designators of common helicopters, for aircraft that send no category. */
static bool is_helicopter_type(const char *t) {
    static const char types[][5] = {"EC20","EC25","EC30","EC35","EC45","EC55","EC75","H160","H125",
        "H135","H145","AS32","AS50","AS55","AS65","A109","A119","A139","A169","A189","AW39","B06",
        "B105","B407","B412","B429","B430","B505","R22","R44","R66","S61","S76","S92","H60","H47",
        "H64","H53","UH1","MI8","MI24","BK17","EN28","EN48","H500"};
    if (!t || !t[0]) return false;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) if (!strcmp(t, types[i])) return true;
    return false;
}
/* ICAO type designators by the shape they get on the radar. */
static const char type_quad[][5] = {      /* four engines */
    "B741","B742","B743","B744","B748","B74S","B74F","VC25","A388","A342","A343","A345","A346",
    "C5M","A124","A225","C17","IL76","IL96","K35R","K35E","C135","E3TF","E3CF","E6","B52","A400"};
static const char type_widebody[][5] = {  /* twin-aisle twins (and trijets) */
    "B762","B763","B764","B772","B773","B77L","B77W","B778","B779","B788","B789","B78X","A332",
    "A333","A338","A339","A359","A35K","A306","A30B","A310","MD11","DC10","KC10","K46","A3ST","E4"};
static const char type_bizjet[][5] = {
    "GLF4","GLF5","GLF6","GL5T","GL7T","GLEX","G280","GALX","CL30","CL35","CL60","C25A","C25B",
    "C25C","C25M","C500","C501","C510","C525","C550","C560","C56X","C650","C680","C68A","C700",
    "C750","E50P","E55P","E545","E550","LJ35","LJ40","LJ45","LJ60","LJ75","FA10","FA20","FA50",
    "FA6X","FA7X","FA8X","F900","F2TH","H25B","HDJT","PC24","ASTR","SF50","EA50","PRM1","BE40"};
static const char type_turboprop[][5] = {
    "AT43","AT45","AT72","AT75","AT76","DH8A","DH8B","DH8C","DH8D","B190","SF34","BE20","BE30",
    "BE9L","BE9T","B350","BE99","D328","JS31","JS32","C212","CN35","C295","C130","C30J","P3",
    "E2","C2","DHC6","SW4","F50","AN26","L410","SH36","P180"};
static const char type_fighter[][5] = {
    "F16","F15","F18H","F18S","F35","F22","A10","F5","T38","T7","EUFI","TOR","RFAL","M2K","MIR2",
    "GRIF","HAWK","F14","B1","F117","A4","AJET","L39","M346"};
static bool type_is(const char *t, const char list[][5], size_t n) {
    if (!t || !t[0]) return false;
    for (size_t i = 0; i < n; i++) if (!strcmp(t, list[i])) return true;
    return false;
}
#define TYPE_IS(t, list) type_is(t, list, sizeof(list) / sizeof(list[0]))
/* The emitter category says roughly what the transmitter is fitted to; the type code then
   picks the shape (four engines, wide-body, business jet...). A plane on the ground shows
   as a plain dot on the radar only. */
static glyph_t glyph_of(const skyframe_aircraft_t *a, bool radar) {
    const char *c = a->category, *t = a->type_code;
    if (c[0] == 'C' || (radar && a->on_ground)) return GLYPH_GROUND;
    if (c[0] == 'B') switch (c[1]) {
        case '1': return GLYPH_GLIDER;
        case '2': return GLYPH_BALLOON;
        case '4': return GLYPH_LIGHT;    /* ultralight */
        case '6': return GLYPH_DRONE;
    }
    if (!strcmp(t, "V22")) return GLYPH_TILTROTOR;
    if ((c[0] == 'A' && c[1] == '7') || is_helicopter_type(t)) return GLYPH_HELI;
    if ((c[0] == 'A' && c[1] == '6') || TYPE_IS(t, type_fighter)) return GLYPH_FIGHTER;
    if (TYPE_IS(t, type_quad)) return GLYPH_JUMBO;
    if (TYPE_IS(t, type_widebody)) return GLYPH_WIDEBODY;
    if (TYPE_IS(t, type_bizjet)) return GLYPH_BIZJET;
    if (TYPE_IS(t, type_turboprop)) return GLYPH_TURBOPROP;
    if (c[0] == 'A') switch (c[1]) {
        case '1': case '2': return GLYPH_LIGHT;
        case '5': return GLYPH_WIDEBODY;   /* heavy of a type not listed: most are twins */
    }
    return GLYPH_JET;
}

/* Radar icon size in percent, in 10% steps by how big the aircraft is: the ADS-B size class,
   split further by type where one class spans very different sizes ("large" runs from a CRJ
   to a 737, "heavy" from a 767 to an A380). The largest still fits the 42 px tap box. */
static bool type_in(const char *t, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (!strcmp(t, list[i])) return true;
    return false;
}
static uint8_t icon_scale(const skyframe_aircraft_t *a) {
    static const char *const regional[] = {   /* regional jets and turboprops */
        "CRJ1","CRJ2","CRJ7","CRJ9","CRJX","E135","E145","E170","E75L","E75S","E190","E195",
        "E290","E295","AT43","AT45","AT72","AT75","AT76","DH8A","DH8B","DH8C","DH8D","SF34","B190"};
    static const char *const jumbo[] = {      /* the largest airliners and freighters */
        "B772","B773","B77L","B77W","B778","B779","B741","B742","B743","B744","B748","B74S",
        "A388","A35K","C5M","A124","A225","IL96","B74F","VC25"};
    const char *c = a->category, *t = a->type_code;
    if (c[0] == 'A') switch (c[1]) {
        case '1': return 60;    /* light, under 15,500 lb: Cessna, Piper, Cirrus */
        case '2': return 70;    /* small: business jets, King Airs */
        case '3': return type_in(t, regional, sizeof(regional) / sizeof(regional[0])) ? 80 : 90;
        case '4': return 100;   /* high-vortex large: the 757 */
        case '5': return type_in(t, jumbo, sizeof(jumbo) / sizeof(jumbo[0])) ? 120 : 110;
        case '7': return 70;    /* helicopter */
    }
    if (type_in(t, jumbo, sizeof(jumbo) / sizeof(jumbo[0]))) return 120;   /* no size class sent */
    return 100;
}

/* Draw glyph g centered on (cx, cy) in absolute coordinates. */
static void draw_glyph(lv_draw_ctx_t *ctx, glyph_t g, lv_coord_t cx, lv_coord_t cy,
                       float heading_deg, float scale, lv_color_t color) {
    #define SC(v) ((lv_coord_t)lroundf((v) * scale))
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.round_start = line.round_end = 1;
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = color;
    if (glyph_rotates(g)) {
        float a = heading_deg * PI_F / 180.f;
        float fx = sinf(a), fy = -cosf(a), sx = cosf(a), sy = sinf(a);
        for (int i = 0; i < glyph_strokes[g].n; i++) {
            const stroke_t *s = &glyph_strokes[g].s[i];
            lv_point_t p1 = {(lv_coord_t)(cx + SC(s->f1 * fx + s->s1 * sx)), (lv_coord_t)(cy + SC(s->f1 * fy + s->s1 * sy))};
            lv_point_t p2 = {(lv_coord_t)(cx + SC(s->f2 * fx + s->s2 * sx)), (lv_coord_t)(cy + SC(s->f2 * fy + s->s2 * sy))};
            line.width = LV_MAX(1, SC(s->w));
            lv_draw_line(ctx, &line, &p1, &p2);
        }
    } else if (g == GLYPH_BALLOON) {     /* round envelope, ropes and basket; always upright */
        fill.radius = LV_RADIUS_CIRCLE;
        lv_area_t env = {cx - SC(7), cy - SC(11), cx + SC(7), cy + SC(3)};
        lv_draw_rect(ctx, &fill, &env);
        line.width = LV_MAX(1, SC(1));
        lv_point_t r[4] = {{cx - SC(5), cy + SC(1)}, {cx - SC(2), cy + SC(7)},
                           {cx + SC(5), cy + SC(1)}, {cx + SC(2), cy + SC(7)}};
        lv_draw_line(ctx, &line, &r[0], &r[1]);
        lv_draw_line(ctx, &line, &r[2], &r[3]);
        fill.radius = SC(1);
        lv_area_t basket = {cx - SC(3), cy + SC(7), cx + SC(3), cy + SC(11)};
        lv_draw_rect(ctx, &fill, &basket);
    } else if (g == GLYPH_DRONE) {       /* X frame with a rotor ring on each arm */
        line.width = LV_MAX(1, SC(2));
        lv_point_t p[4] = {{cx - SC(6), cy - SC(6)}, {cx + SC(6), cy + SC(6)},
                           {cx - SC(6), cy + SC(6)}, {cx + SC(6), cy - SC(6)}};
        lv_draw_line(ctx, &line, &p[0], &p[1]);
        lv_draw_line(ctx, &line, &p[2], &p[3]);
        lv_draw_arc_dsc_t ring;
        lv_draw_arc_dsc_init(&ring);
        ring.color = color;
        ring.width = LV_MAX(1, SC(2));
        for (int k = 0; k < 4; k++) {
            lv_point_t c = {cx + (k & 1 ? SC(7) : -SC(7)), cy + (k & 2 ? SC(7) : -SC(7))};
            lv_draw_arc(ctx, &ring, &c, (uint16_t)SC(3), 0, 360);
        }
    } else {                             /* ground vehicle, or anything on the ground */
        fill.radius = SC(3);
        lv_area_t box = {cx - SC(4), cy - SC(4), cx + SC(4), cy + SC(4)};
        lv_draw_rect(ctx, &fill, &box);
    }
    #undef SC
}

/* ---- Aircraft details view ----------------------------------------------
   A scrollable round page. First screen: photo, callsign, model, airline, route
   and live numbers. Scroll down for the airframe record. A tap (without
   scrolling) closes it. Live ADS-B data shows at once; the database record and
   photo fill in as they arrive. */
#define DEG "\xC2\xB0"
#define WHITE_TXT lv_color_hex(0xEAF7EF)
#define MUTED_TXT lv_color_hex(0x8FB8A0)
#define CARD_BG   lv_color_hex(0x0D2418)
#define DIVIDER   lv_color_hex(0x1C3B2A)
#define PHOTO_X ((SIZE - SKYFRAME_PHOTO_W) / 2)
#define PHOTO_Y 38                        /* as high as the round glass allows with radius 18 */
#define PHOTO_RADIUS 18
enum { ST_ALT, ST_SPEED, ST_VRATE, ST_HEADING, ST_DIST, ST_SQUAWK, ST_COUNT };
enum { IT_WHY, IT_MODEL, IT_TYPE, IT_REG, IT_OPERATOR, IT_COUNTRY, IT_BUILT, IT_FROM, IT_TO, IT_HEX, IT_CATEGORY, IT_COUNT };
static lv_obj_t *d_photo_msg, *d_badge, *d_glyph;
static glyph_t d_glyph_kind;
static lv_obj_t *d_callsign, *d_model, *d_airline;
static lv_obj_t *d_orig, *d_dest, *d_orig_city, *d_dest_city, *d_route_line, *d_route_msg;
static lv_obj_t *d_stat[ST_COUNT];
static lv_obj_t *d_lookup, *d_item[IT_COUNT], *d_item_val[IT_COUNT];
/* Two photo layers: a typical photo of the type (from the card) shows at once, then the photo
   of this exact airframe fades in over it when it has downloaded. */
#define PHOTO_FADE_MS 600
static lv_obj_t *d_photo[2];
static uint16_t *d_pixels[2];
static lv_img_dsc_t d_img[2];
static int d_front = -1;                 /* layer fully shown, -1 none */
static int d_fading = -1;                /* layer fading in, -1 none */
static void drop_layer(int i) {          /* hide a layer and free its pixels */
    lv_obj_add_flag(d_photo[i], LV_OBJ_FLAG_HIDDEN);
    lv_img_cache_invalidate_src(&d_img[i]);
    free(d_pixels[i]); d_pixels[i] = NULL;
}
static void fade_done(void) {            /* the fading layer is now the photo; the other goes */
    int in = d_fading;
    if (in < 0) return;
    d_fading = -1;
    lv_obj_set_style_img_opa(d_photo[in], LV_OPA_COVER, 0);
    if (d_front >= 0 && d_front != in) drop_layer(d_front);
    d_front = in;
}
static void fade_ready(lv_anim_t *a) { (void)a; fade_done(); }
static void fade_opa(void *img, int32_t v) { lv_obj_set_style_img_opa((lv_obj_t *)img, (lv_opa_t)v, 0); }
static void fade_finish(void) {          /* a new photo or a new plane: complete any fade now */
    if (d_fading >= 0) { lv_anim_del(d_photo[d_fading], fade_opa); fade_done(); }
}
static bool photo_shown(void) { return d_front >= 0 || d_fading >= 0; }

static lv_obj_t *text(lv_obj_t *parent, const char *s, int x, int y, int w,
                      lv_color_t c, const lv_font_t *font, bool one_line) {
    lv_obj_t *o = new_label(parent, s, x, y, c, font);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    if (one_line) {
        lv_obj_set_height(o, lv_font_get_line_height(font ? font : LV_FONT_DEFAULT));
        lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    }
    return o;
}
static void plain_box(lv_obj_t *o) {
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
/* Registry text is often ALL CAPS: "BOEING 767-400" -> "Boeing 767-400".
   Mixed-case text is kept; words with digits stay as-is; cut at the first comma. */
static void tidy_name(const char *in, char *out, size_t cap) {
    snprintf(out, cap, "%s", in ? in : "");
    char *comma = strchr(out, ',');
    if (comma) *comma = 0;
    for (const char *p = out; *p; p++) if (islower((unsigned char)*p)) return;
    for (char *w = out; *w; ) {
        char *end = w; bool digits = false;
        while (*end && *end != ' ') { if (isdigit((unsigned char)*end)) digits = true; end++; }
        if (!digits) for (char *p = w + 1; p < end; p++) *p = (char)tolower((unsigned char)*p);
        w = *end ? end + 1 : end;
    }
}
static const char *compass(float deg) {
    static const char *names[8] = {"N","NE","E","SE","S","SW","W","NW"};
    return names[((int)lroundf(fmodf(deg + 360.f, 360.f) / 45.f)) % 8];
}
static void with_commas(int v, char *buf, size_t cap) {
    int mag = v < 0 ? -v : v;
    if (mag >= 1000) snprintf(buf, cap, "%d,%03d", mag / 1000, mag % 1000);
    else snprintf(buf, cap, "%d", mag);
}
static void category_text(const char *c, char *buf, size_t cap) {
    static const struct { const char *code, *name; } map[] = {
        {"A1","Light"},{"A2","Small"},{"A3","Large"},{"A4","Large, high vortex"},{"A5","Heavy"},
        {"A6","High performance"},{"A7","Helicopter"},{"B1","Glider"},{"B2","Balloon / airship"},
        {"B4","Ultralight"},{"B6","Drone"},{"C1","Emergency vehicle"},{"C2","Service vehicle"}};
    buf[0] = 0;
    if (!c || !c[0]) return;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        if (!strcmp(c, map[i].code)) { snprintf(buf, cap, "%s (%s)", map[i].name, c); return; }
    snprintf(buf, cap, "%s", c);
}
static void set_item(int i, const char *value) {   /* also called every 250 ms: change-only */
    bool show = value && value[0];
    if (show && strcmp(lv_label_get_text(d_item_val[i]), value)) lv_label_set_text(d_item_val[i], value);
    if (show == lv_obj_has_flag(d_item[i], LV_OBJ_FLAG_HIDDEN)) {
        if (show) lv_obj_clear_flag(d_item[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(d_item[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void show_route(const char *msg) {
    bool codes = msg == NULL;
    lv_obj_t *parts[] = {d_orig, d_dest, d_orig_city, d_dest_city, d_route_line};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (codes) lv_obj_clear_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (codes) lv_obj_add_flag(d_route_msg, LV_OBJ_FLAG_HIDDEN);
    else { lv_label_set_text(d_route_msg, msg); lv_obj_clear_flag(d_route_msg, LV_OBJ_FLAG_HIDDEN); }
}
static void set_text_if_changed(lv_obj_t *label, const char *text);
static void show_obj(lv_obj_t *o, bool show);
/* Live numbers; runs every 250 ms while the page is open, so only changes are applied. */
static void refresh_stats(const skyframe_aircraft_t *a) {
    char buf[32], num[16];
    format_altitude(a, buf, sizeof(buf));
    set_text_if_changed(d_stat[ST_ALT], buf);
    snprintf(buf, sizeof(buf), "%.0f kt", a->speed_knots);
    set_text_if_changed(d_stat[ST_SPEED], buf);
    if (!a->has_vert_rate || a->on_ground) snprintf(buf, sizeof(buf), "--");
    else if (abs(a->vert_rate_fpm) < 100) snprintf(buf, sizeof(buf), "Level");
    else { with_commas(a->vert_rate_fpm, num, sizeof(num));
           snprintf(buf, sizeof(buf), "%s%s fpm", a->vert_rate_fpm > 0 ? "+" : "-", num); }
    set_text_if_changed(d_stat[ST_VRATE], buf);
    snprintf(buf, sizeof(buf), "%.0f" DEG " %s", a->heading_deg, compass(a->heading_deg));
    set_text_if_changed(d_stat[ST_HEADING], buf);
    snprintf(buf, sizeof(buf), "%.1f mi %s", a->distance_miles, compass(a->bearing_deg));
    set_text_if_changed(d_stat[ST_DIST], buf);
    set_text_if_changed(d_stat[ST_SQUAWK], a->squawk[0] ? a->squawk : "--");
    /* Badge over the photo: emergency, VIP, military, notable (updates live, e.g. a new squawk). */
    char badge[40], why[128];
    traffic_kind_t kind = classify_why(a, badge, sizeof(badge), why, sizeof(why));
    set_item(IT_WHY, why);   /* first entry of the record; hidden for ordinary traffic */
    if (badge[0]) {
        static int badge_kind = -1;
        set_text_if_changed(d_badge, badge);
        if (badge_kind != (int)kind) {
            badge_kind = (int)kind;
            lv_obj_set_style_bg_color(d_badge, lv_color_hex(kind == KIND_NORMAL ? 0x8FB8A0 : kind_color[kind]), 0);
        }
    }
    show_obj(d_badge, badge[0] != 0);
}
static void photo_message(const char *msg) {
    for (int i = 0; i < 2; i++) lv_obj_add_flag(d_photo[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(d_photo_msg, msg);
    lv_obj_clear_flag(d_photo_msg, LV_OBJ_FLAG_HIDDEN);
}

static void sweep_run(bool run);
static void redraw_planes(void);
static void close_details(lv_event_t *event) {
    (void)event;
    skyframe_ui_close_details();
}

static void open_details(lv_event_t *event) {
    uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (i >= traffic_count) return;
    const skyframe_aircraft_t *a = &traffic[i];
    snprintf(selected_icao, sizeof(selected_icao), "%s", a->icao);
    char buf[64], tidy[64];

    /* Previous aircraft's photos go away; the hidden images no longer draw them. */
    fade_finish();
    photo_message("Loading photo...");
    for (int k = 0; k < 2; k++) drop_layer(k);
    d_front = -1;

    glyph_t g = glyph_of(a, false);      /* placeholder matches the kind of aircraft */
    if (g != d_glyph_kind) { d_glyph_kind = g; lv_obj_invalidate(d_glyph); }
    lv_label_set_text(d_callsign, a->callsign[0] ? a->callsign : "UNKNOWN");
    tidy_name(a->aircraft_type[0] && strcmp(a->aircraft_type, "Unknown aircraft") ? a->aircraft_type : a->type_code, tidy, sizeof(tidy));
    lv_label_set_text(d_model, tidy[0] ? tidy : "Aircraft");
    set_item(IT_MODEL, tidy);
    tidy_name(a->operator_name, tidy, sizeof(tidy));
    lv_label_set_text(d_airline, tidy);
    set_item(IT_OPERATOR, tidy);
    show_route("Looking up route...");

    set_item(IT_TYPE, a->type_code);
    set_item(IT_REG, a->registration);
    set_item(IT_COUNTRY, NULL);
    set_item(IT_BUILT, a->year);
    set_item(IT_FROM, NULL);
    set_item(IT_TO, NULL);
    size_t k = 0;
    for (; a->icao[k] && k < sizeof(buf) - 1; k++) buf[k] = (char)toupper((unsigned char)a->icao[k]);
    buf[k] = 0;
    set_item(IT_HEX, buf);
    category_text(a->category, buf, sizeof(buf));
    set_item(IT_CATEGORY, buf);
    lv_label_set_text(d_lookup, "Looking up aircraft record...");
    lv_obj_clear_flag(d_lookup, LV_OBJ_FLAG_HIDDEN);

    refresh_stats(a);
    lv_obj_scroll_to_y(detail_screen, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(detail_screen, LV_OBJ_FLAG_HIDDEN);
    sweep_run(false);                    /* hidden behind the page anyway */
    if (on_selected) on_selected(a);
}

/* Large outline of the aircraft kind, shown in the photo frame until a photo arrives. */
static void detail_glyph_draw(lv_event_t *e) {
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    draw_glyph(lv_event_get_draw_ctx(e), d_glyph_kind, (lv_coord_t)((c.x1 + c.x2) / 2), c.y1 + 66,
               0.f, 3.5f, DIM_GREEN);
}
/* Route line between the airport codes: end dots, dashes and a small jet heading east. */
static void route_line_draw(lv_event_t *e) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    lv_coord_t y = (lv_coord_t)((c.y1 + c.y2) / 2), mid = (lv_coord_t)((c.x1 + c.x2) / 2);
    lv_draw_line_dsc_t dash;
    lv_draw_line_dsc_init(&dash);
    dash.color = DIM_TEXT;
    dash.width = 2;
    dash.dash_width = 4;
    dash.dash_gap = 4;
    lv_point_t l1 = {c.x1 + 10, y}, l2 = {mid - 16, y}, r1 = {mid + 16, y}, r2 = {c.x2 - 10, y};
    lv_draw_line(ctx, &dash, &l1, &l2);
    lv_draw_line(ctx, &dash, &r1, &r2);
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = DIM_TEXT;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t d1 = {c.x1 + 1, y - 3, c.x1 + 7, y + 3}, d2 = {c.x2 - 7, y - 3, c.x2 - 1, y + 3};
    lv_draw_rect(ctx, &dot, &d1);
    lv_draw_rect(ctx, &dot, &d2);
    draw_glyph(ctx, GLYPH_JET, mid, y, 90.f, 0.9f, GREEN);
}
static lv_obj_t *card_box(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
    lv_obj_t *o = lv_obj_create(parent); plain_box(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, CARD_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* Layout of the first screen (y in px; the glass is a 480 px circle, so every box is
   checked against it, corners included):
     38-194  photo, rounded, kind badge on top
    200-264  callsign (24), model (16), airline (14)
    272-326  route card: codes (24) with a dashed line and jet between, cities under
    334-423  six live numbers as two rows of three tiles
    428-472  "swipe up" hint and a Close pill (the whole page closes on a tap) */
#define TILE_W 100
#define TILE_H 42
#define TILE_GAP 5
#define TILE_X ((SIZE - 3 * TILE_W - 2 * TILE_GAP) / 2)
#define TILE_Y 334
#define RECORD_W 320
static void build_details(lv_obj_t *parent) {
    detail_screen = lv_obj_create(parent);
    lv_obj_set_size(detail_screen, SIZE, SIZE);
    lv_obj_center(detail_screen);
    lv_obj_set_style_bg_color(detail_screen, BG, 0);
    lv_obj_set_style_bg_opa(detail_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail_screen, 0, 0);
    lv_obj_set_style_pad_all(detail_screen, 0, 0);
    lv_obj_set_scroll_dir(detail_screen, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(detail_screen, close_details, LV_EVENT_CLICKED, NULL);

    /* Photo card with an outline of the aircraft kind shown until (or unless) a photo arrives. */
    lv_obj_t *card = card_box(detail_screen, PHOTO_X, PHOTO_Y, SKYFRAME_PHOTO_W, SKYFRAME_PHOTO_H, PHOTO_RADIUS);
    lv_obj_set_style_clip_corner(card, true, 0);
    d_glyph = lv_obj_create(card); plain_box(d_glyph);
    lv_obj_set_size(d_glyph, SKYFRAME_PHOTO_W, SKYFRAME_PHOTO_H);
    lv_obj_add_event_cb(d_glyph, detail_glyph_draw, LV_EVENT_DRAW_MAIN, NULL);
    d_photo_msg = text(card, "", 0, 126, SKYFRAME_PHOTO_W, MUTED_TXT, LV_FONT_DEFAULT, true);
    for (int i = 0; i < 2; i++) {
        d_photo[i] = lv_img_create(card);
        lv_obj_set_pos(d_photo[i], 0, 0);
        lv_obj_add_flag(d_photo[i], LV_OBJ_FLAG_HIDDEN);
    }
    d_badge = lv_label_create(card);   /* above the photo, top center: clear of the glass edge */
    lv_obj_align(d_badge, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_opa(d_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d_badge, 10, 0);
    lv_obj_set_style_pad_hor(d_badge, 10, 0);
    lv_obj_set_style_pad_ver(d_badge, 3, 0);
    lv_obj_set_style_text_color(d_badge, BG, 0);
    lv_obj_set_style_text_font(d_badge, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(d_badge, LV_OBJ_FLAG_HIDDEN);

    /* Identity: big callsign, model in green, airline */
    d_callsign = text(detail_screen, "", 60, 200, 360, WHITE_TXT, &lv_font_montserrat_24, true);
    d_model = text(detail_screen, "", 60, 228, 360, GREEN, &lv_font_montserrat_16, true);
    d_airline = text(detail_screen, "", 70, 248, 340, MUTED_TXT, LV_FONT_DEFAULT, true);

    /* Route card: IAD - - plane - - LHR, cities underneath */
    lv_obj_t *route = card_box(detail_screen, 70, 272, 340, 54, 14);
    d_orig = text(route, "", 6, 4, 110, WHITE_TXT, &lv_font_montserrat_24, true);
    d_dest = text(route, "", 224, 4, 110, WHITE_TXT, &lv_font_montserrat_24, true);
    d_orig_city = text(route, "", 6, 33, 140, MUTED_TXT, &lv_font_montserrat_12, true);
    d_dest_city = text(route, "", 194, 33, 140, MUTED_TXT, &lv_font_montserrat_12, true);
    d_route_line = lv_obj_create(route); plain_box(d_route_line);
    lv_obj_set_pos(d_route_line, 112, 3);
    lv_obj_set_size(d_route_line, 116, 29);
    lv_obj_add_event_cb(d_route_line, route_line_draw, LV_EVENT_DRAW_MAIN, NULL);
    d_route_msg = text(route, "", 10, 18, 320, MUTED_TXT, LV_FONT_DEFAULT, true);

    /* Live numbers: two rows of three tiles, an icon and caption over the value */
    static const char *names[ST_COUNT] = {LV_SYMBOL_UPLOAD " ALTITUDE", LV_SYMBOL_CHARGE " SPEED",
        LV_SYMBOL_BARS " VERTICAL", LV_SYMBOL_GPS " HEADING", LV_SYMBOL_HOME " DISTANCE", LV_SYMBOL_WIFI " SQUAWK"};
    for (int i = 0; i < ST_COUNT; i++) {
        lv_obj_t *tile = card_box(detail_screen, TILE_X + (i % 3) * (TILE_W + TILE_GAP),
                                  TILE_Y + (i / 3) * (TILE_H + TILE_GAP), TILE_W, TILE_H, 10);
        text(tile, names[i], 0, 4, TILE_W, DIM_TEXT, &lv_font_montserrat_12, true);
        d_stat[i] = text(tile, "--", 2, 20, TILE_W - 4, WHITE_TXT, &lv_font_montserrat_16, true);
    }
    text(detail_screen, LV_SYMBOL_DOWN "  Swipe up for more", 140, 428, 200, DIM_TEXT, &lv_font_montserrat_12, true);
    lv_obj_t *close = card_box(detail_screen, (SIZE - 110) / 2, 446, 110, 26, 13);
    lv_obj_set_style_border_color(close, DIM_GREEN, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    /* The pill only shows where to tap: any tap on the page closes it. */
    text(close, LV_SYMBOL_CLOSE "  Close", 0, 4, 108, WHITE_TXT, LV_FONT_DEFAULT, true);

    /* Second screen: the airframe record as one card of label / value rows. */
    lv_obj_t *col = lv_obj_create(detail_screen); plain_box(col);
    lv_obj_set_pos(col, (SIZE - RECORD_W) / 2, 490);
    lv_obj_set_size(col, RECORD_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *rec = lv_obj_create(col); plain_box(rec);
    lv_obj_set_size(rec, RECORD_W, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(rec, 18, 0);
    lv_obj_set_style_bg_color(rec, CARD_BG, 0);
    lv_obj_set_style_bg_opa(rec, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(rec, 16, 0);
    lv_obj_set_style_pad_ver(rec, 12, 0);
    lv_obj_set_flex_flow(rec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rec, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *head = lv_label_create(rec);
    lv_label_set_text(head, "AIRCRAFT RECORD");
    lv_obj_set_style_text_color(head, GREEN, 0);
    lv_obj_set_style_pad_bottom(head, 8, 0);
    d_lookup = lv_label_create(rec);
    lv_obj_set_style_text_color(d_lookup, MUTED_TXT, 0);
    lv_obj_set_style_text_font(d_lookup, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_bottom(d_lookup, 8, 0);
    static const char *item_names[IT_COUNT] = {"WHY IT'S HIGHLIGHTED", "MODEL", "TYPE CODE", "REGISTRATION", "OPERATOR",
        "REGISTERED IN", "BUILT", "FROM", "TO", "ICAO ADDRESS", "CATEGORY"};
    const int inner = RECORD_W - 32;
    for (int i = 0; i < IT_COUNT; i++) {
        /* A divider above each row; the "why" note is a full-width paragraph, the rest are
           label left, value right (long values wrap). */
        bool para = i == IT_WHY;
        d_item[i] = lv_obj_create(rec); plain_box(d_item[i]);
        lv_obj_set_size(d_item[i], inner, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(d_item[i], 9, 0);
        lv_obj_set_style_border_side(d_item[i], LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(d_item[i], 1, 0);
        lv_obj_set_style_border_color(d_item[i], DIVIDER, 0);
        lv_obj_set_flex_flow(d_item[i], para ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(d_item[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(d_item[i], 4, 0);
        lv_obj_set_style_pad_column(d_item[i], 8, 0);
        lv_obj_t *name = lv_label_create(d_item[i]);
        lv_label_set_text(name, item_names[i]);
        lv_obj_set_style_text_color(name, DIM_TEXT, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        if (!para) lv_obj_set_width(name, 104);
        d_item_val[i] = lv_label_create(d_item[i]);
        lv_obj_set_style_text_color(d_item_val[i], WHITE_TXT, 0);
        if (para) {
            lv_obj_set_width(d_item_val[i], inner);
            lv_obj_set_style_text_font(d_item_val[i], LV_FONT_DEFAULT, 0);
        } else {
            lv_obj_set_flex_grow(d_item_val[i], 1);
            lv_obj_set_style_text_align(d_item_val[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(d_item_val[i], &lv_font_montserrat_16, 0);
        }
        lv_obj_add_flag(d_item[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *spacer = lv_obj_create(col); plain_box(spacer);
    lv_obj_set_size(spacer, 10, 150);   /* lets the last rows scroll up to mid-screen */
    lv_obj_add_flag(detail_screen, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_point_t *pts, lv_color_t color, int width) {
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}
static void show_obj(lv_obj_t *o, bool show) {   /* only touch the flag when it changes */
    if (show == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}
static void set_text_if_changed(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

/* Per-plane values last applied, so a redraw only touches what changed: the glyph's shape,
   color and whole-degree heading. A change just invalidates the plane's own 42x42 box. */
static struct { int8_t kind, align; uint8_t glyph, scale; int16_t heading; } plane_look[SKYFRAME_MAX_AIRCRAFT];
static void plane_draw(lv_event_t *e) {
    uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    /* Ordinary traffic on the ground is a dim dot; anything special keeps its color. */
    bool parked = plane_look[i].glyph == GLYPH_GROUND && plane_look[i].kind == KIND_NORMAL;
    draw_glyph(lv_event_get_draw_ctx(e), (glyph_t)plane_look[i].glyph, c.x1 + PLANE_BOX / 2, c.y1 + PLANE_BOX / 2,
               (float)plane_look[i].heading, plane_look[i].scale / 100.f,
               lv_color_hex(parked ? GROUND_DOT : kind_color[plane_look[i].kind]));
}
static void box_of(lv_obj_t *o, box_t *b) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) { *b = (box_t){0, 0, 0, 0}; return; }
    lv_area_t a; lv_obj_get_coords(o, &a);
    *b = (box_t){a.x1, a.y1, a.x2 + 1, a.y2 + 1};
}

static int overlap_area(const box_t *a, const box_t *b) {
    int w = LV_MIN(a->x2, b->x2) - LV_MAX(a->x1, b->x1), h = LV_MIN(a->y2, b->y2) - LV_MAX(a->y1, b->y1);
    return w > 0 && h > 0 ? w * h : 0;
}
#define ICON_HALF 13                     /* the drawn glyph, inside the 42 px tap box */
static void redraw_planes(void) {
    /* Traffic is nearest-first, so nearer planes get the preferred label spots. Labels also
       keep clear of the clock, the status line, the airport label and other planes' icons. */
    box_t placed[SKYFRAME_MAX_AIRCRAFT + 7];
    box_t icons[SKYFRAME_MAX_AIRCRAFT];
    int px[SKYFRAME_MAX_AIRCRAFT], py[SKYFRAME_MAX_AIRCRAFT];
    int placed_count = 0;
    box_of(clock_label, &placed[placed_count++]);
    box_of(status_label, &placed[placed_count++]);
    box_of(coords_lat, &placed[placed_count++]);
    box_of(coords_lon, &placed[placed_count++]);
    box_of(range_label, &placed[placed_count++]);
    box_of(wifi_icon, &placed[placed_count++]);
    box_of(airport_text, &placed[placed_count++]);
    /* First every plane's position, so each label can avoid all the icons. */
    bool visible[SKYFRAME_MAX_AIRCRAFT];
    uint8_t scale[SKYFRAME_MAX_AIRCRAFT];
    glyph_t glyphs[SKYFRAME_MAX_AIRCRAFT];
    for (uint8_t i = 0; i < SKYFRAME_MAX_AIRCRAFT; i++) {
        visible[i] = i < traffic_count && traffic[i].distance_miles <= display_radius;
        icons[i] = (box_t){0, 0, 0, 0};
        if (!visible[i]) continue;
        point_from_bearing(traffic[i].distance_miles, traffic[i].bearing_deg, &px[i], &py[i]);
        glyphs[i] = glyph_of(&traffic[i], true);
        scale[i] = glyphs[i] == GLYPH_GROUND ? 70 : icon_scale(&traffic[i]);   /* ground: a small plain dot */
        int half = ICON_HALF * scale[i] / 100;
        icons[i] = (box_t){px[i] - half, py[i] - half, px[i] + half, py[i] + half};
    }
    for (uint8_t i = 0; i < SKYFRAME_MAX_AIRCRAFT; i++) {
        if (!visible[i]) {
            show_obj(planes[i], false);
            show_obj(plane_labels[i], false);
            continue;
        }
        int x = px[i], y = py[i];
        traffic_kind_t kind = classify(&traffic[i], NULL, 0);
        glyph_t glyph = glyphs[i];
        int16_t heading = glyph_rotates(glyph)   /* upright shapes ignore heading changes */
            ? (int16_t)(((int)lroundf(traffic[i].heading_deg) % 360 + 360) % 360) : 0;
        if (plane_look[i].kind != (int8_t)kind)
            lv_obj_set_style_text_color(plane_labels[i], lv_color_hex(kind_color[kind]), 0);
        bool restyle = plane_look[i].kind != (int8_t)kind || plane_look[i].glyph != glyph ||
                       plane_look[i].heading != heading || plane_look[i].scale != scale[i];
        plane_look[i].kind = (int8_t)kind;
        plane_look[i].glyph = (uint8_t)glyph;
        plane_look[i].scale = scale[i];
        plane_look[i].heading = heading;
        lv_obj_set_pos(planes[i], x - PLANE_BOX / 2, y - PLANE_BOX / 2);   /* no-op when unmoved */
        if (restyle) lv_obj_invalidate(planes[i]);
        show_obj(planes[i], true);
        /* On the ground: just the dot (still tappable for details); the airport label counts them. */
        if (glyph == GLYPH_GROUND) { show_obj(plane_labels[i], false); continue; }

        /* Data block: callsign over altitude, under the plane if that's clear. Otherwise the
           first clear spot around it (above, the sides, the diagonals, then a row further out);
           when traffic is so dense that none is clear, the spot that overlaps the least. Spots
           clipped by the round glass are skipped. */
        char alt[20], text[40];
        format_altitude(&traffic[i], alt, sizeof(alt));
        snprintf(text, sizeof(text), "%.11s\n%.19s", traffic[i].callsign[0] ? traffic[i].callsign : "UNKNOWN", alt);
        set_text_if_changed(plane_labels[i], text);
        const int W = PLANE_LABEL_W, H = PLANE_LABEL_H;
        enum { SPOTS = 10 };
        const box_t spots[SPOTS] = {
            {x - W/2,     y + 13,          x + W/2,      y + 13 + H},       /* below */
            {x - W/2,     y - 13 - H,      x + W/2,      y - 13},           /* above */
            {x + 15,      y - H/2,         x + 15 + W,   y + H/2},          /* right */
            {x - 15 - W,  y - H/2,         x - 15,       y + H/2},          /* left */
            {x + 10,      y + 10,          x + 10 + W,   y + 10 + H},       /* below right */
            {x - 10 - W,  y + 10,          x - 10,       y + 10 + H},       /* below left */
            {x + 10,      y - 10 - H,      x + 10 + W,   y - 10},           /* above right */
            {x - 10 - W,  y - 10 - H,      x - 10,       y - 10},           /* above left */
            {x - W/2,     y + 15 + H,      x + W/2,      y + 15 + 2 * H},   /* further below */
            {x - W/2,     y - 15 - 2 * H,  x + W/2,      y - 15 - H},       /* further above */
        };
        static const lv_text_align_t aligns[SPOTS] = {
            LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_RIGHT,
            LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_RIGHT, LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_RIGHT,
            LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_CENTER};
        int pick = -1, best = INT_MAX;
        for (int s = 0; s < SPOTS; s++) {
            if (!box_inside_circle(&spots[s])) continue;
            int cost = 0;   /* overlap with labels, the clock etc., and other planes' icons */
            for (int k = 0; k < placed_count; k++) cost += overlap_area(&spots[s], &placed[k]);
            for (int k = 0; k < SKYFRAME_MAX_AIRCRAFT; k++)
                if (k != i && visible[k]) cost += overlap_area(&spots[s], &icons[k]);
            if (cost < best) { best = cost; pick = s; }
            if (!cost) break;   /* spots are in order of preference */
        }
        if (pick < 0) pick = 0;   /* nowhere fully inside the glass: under the plane */
        if (plane_look[i].align != aligns[pick]) {   /* alignment change re-lays-out the label */
            plane_look[i].align = (int8_t)aligns[pick];
            lv_obj_set_style_text_align(plane_labels[i], aligns[pick], 0);
        }
        lv_obj_set_pos(plane_labels[i], spots[pick].x1, spots[pick].y1);
        show_obj(plane_labels[i], true);
        placed[placed_count++] = spots[pick];
    }
}

/* Airport marker: a diamond inside a ring, in the label's gold. */
static void airport_pin_draw(lv_event_t *e) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_coord_t cx = (lv_coord_t)((a.x1 + a.x2) / 2), cy = (lv_coord_t)((a.y1 + a.y2) / 2);
    lv_color_t gold = lv_color_hex(0xFFD184);
    lv_draw_arc_dsc_t ring; lv_draw_arc_dsc_init(&ring);
    ring.color = gold; ring.width = 2;
    lv_point_t c = {cx, cy};
    lv_draw_arc(ctx, &ring, &c, AIRPORT_PIN / 2, 0, 360);
    lv_draw_rect_dsc_t fill; lv_draw_rect_dsc_init(&fill);
    fill.bg_color = gold;
    const lv_coord_t d = 4;
    lv_point_t p[4] = {{cx, (lv_coord_t)(cy - d)}, {(lv_coord_t)(cx + d), cy}, {cx, (lv_coord_t)(cy + d)}, {(lv_coord_t)(cx - d), cy}};
    lv_draw_polygon(ctx, &fill, p, 4);
}
/* "BWI", or "BWI  14" (aircraft on the ground there); right of the pin, or left of it when
   that runs off the glass. */
static void place_airport_text(void) {
    if (!airport_text || airport_x < 0) return;
    char text[48];
    if (airport_ground) snprintf(text, sizeof(text), "BWI  #8FA3BF %u#", airport_ground);
    else snprintf(text, sizeof(text), "BWI");
    set_text_if_changed(airport_text, text);
    lv_obj_update_layout(airport_text);
    int w = lv_obj_get_width(airport_text);
    int x = airport_x + AIRPORT_PIN / 2 + 5;
    int dy = airport_y - CY, room = VISIBLE_R * VISIBLE_R - dy * dy;
    int half = room > 0 ? (int)sqrtf((float)room) : 0;
    if (x + w > CX + half - 6) x = airport_x - AIRPORT_PIN / 2 - 5 - w;
    lv_obj_set_pos(airport_text, x, airport_y - 9);
}
void skyframe_ui_set_airport_ground(unsigned count) {
    if (count == airport_ground) return;
    airport_ground = count;
    place_airport_text();
}
/* The clock lives at the top, the status line at the bottom. An airport marker that sits
   where the clock is (one a few miles due north) would cover it for good, unlike a passing
   plane: then the two swap places. */
#define CLOCK_TOP_Y 56
#define STATUS_BOTTOM_Y 396
static void place_clock(void) {
    if (!clock_label || !status_label) return;
    bool swap = false;
    if (airport_x >= 0) {
        box_t a = {airport_x - AIRPORT_PIN / 2, airport_y - AIRPORT_PIN / 2,
                   airport_x + AIRPORT_PIN / 2, airport_y + AIRPORT_PIN / 2};
        box_t t;
        box_of(airport_text, &t);
        a.x1 = LV_MIN(a.x1, t.x1); a.y1 = LV_MIN(a.y1, t.y1);
        a.x2 = LV_MAX(a.x2, t.x2); a.y2 = LV_MAX(a.y2, t.y2);
        /* the clock's spot ("12:59 PM") with a clear margin: near it reads as cramped too */
        box_t top = {CX - 75, CLOCK_TOP_Y - 16, CX + 75, CLOCK_TOP_Y + 36};
        box_t bottom = {CX - 75, STATUS_BOTTOM_Y - 16, CX + 75, STATUS_BOTTOM_Y + 36};
        swap = overlap_area(&a, &top) > 0 && overlap_area(&a, &bottom) == 0;
    }
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, swap ? STATUS_BOTTOM_Y : CLOCK_TOP_Y);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, swap ? CLOCK_TOP_Y : STATUS_BOTTOM_Y);
}
static float airport_dist = NAN, airport_bearing;   /* kept for a new radius */
static bool airport_wanted;
void skyframe_ui_set_airport(float distance_miles, float bearing_deg, bool visible) {
    airport_dist = distance_miles; airport_bearing = bearing_deg; airport_wanted = visible;
    if (!root || !airport_pin || !airport_text) return;
    visible = visible && isfinite(distance_miles) && isfinite(bearing_deg) &&
              distance_miles > 0.05f && distance_miles <= display_radius;
    if (!visible) {
        lv_obj_add_flag(airport_pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(airport_text, LV_OBJ_FLAG_HIDDEN);
        airport_x = -1;
        place_clock();
        return;
    }
    int x, y;
    point_from_bearing(distance_miles, bearing_deg, &x, &y);
    lv_obj_set_pos(airport_pin, x - AIRPORT_PIN / 2, y - AIRPORT_PIN / 2);
    airport_x = x; airport_y = y;
    lv_obj_clear_flag(airport_pin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(airport_text, LV_OBJ_FLAG_HIDDEN);
    place_airport_text();
    lv_obj_update_layout(airport_text);
    place_clock();
}

void skyframe_ui_set_aircraft(const skyframe_aircraft_t *aircraft, uint8_t count) {
    if (!root) return;
    if (!aircraft) count = 0;
    if (count > SKYFRAME_MAX_AIRCRAFT) count = SKYFRAME_MAX_AIRCRAFT;
    if (count) memcpy(traffic, aircraft, count * sizeof(*aircraft));
    traffic_count = count;
    /* The details page covers the radar: skip drawing planes nobody can see. */
    if (!skyframe_ui_details_open()) redraw_planes();
    else {
        /* Re-find by ICAO address: nearest-first order changes every update. If the plane
           has left the feed, keep its last numbers rather than snapping the page shut. */
        for (int j = 0; j < count; j++)
            if (!strcmp(traffic[j].icao, selected_icao)) { refresh_stats(&traffic[j]); break; }
    }
}

/* Radar sweep, paced by the panel's own refreshes: it moves by the same angle every second
   refresh (~32 steps a second) and is drawn at once, so each step shows on the very next
   refresh. Even steps look smoother than a faster but irregular rate. Without a frame
   clock it falls back to timing by the millisecond clock. It is drawn by a transparent
   full-radar object; each move invalidates only small boxes along the old and new beam. */
#define SWEEP_MS_PER_TURN 5000U
#define SWEEP_FRAMES_PER_TURN 324U   /* ~5 s at the panel's ~64.7 Hz */
#define SWEEP_FRAMES_PER_STEP 2U
#define SWEEP_PERIOD_MS 1            /* polls the frame clock on every UI loop pass (woken at each refresh) */
#define SWEEP_WIDTH 2
#define SWEEP_SEGS 6                 /* radial bands, each invalidated as its own box */
#define TRAIL_DEG 30.f               /* afterglow behind the beam, fading out */
#define TRAIL_STEPS 10
static uint32_t (*frame_clock)(void);
static uint32_t sweep_frame;
#define SWEEP_COLOR lv_color_hex(0x48B875)
static float sweep_deg;              /* beam angle, clockwise from north */
static lv_point_t polar(float deg, float r) {
    float a = deg * PI_F / 180.f;
    return (lv_point_t){(lv_coord_t)(CX + lroundf(sinf(a) * r)), (lv_coord_t)(CY - lroundf(cosf(a) * r))};
}
static void sweep_draw(lv_event_t *e) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    /* Afterglow: a fan of thin wedges, brightest next to the beam. */
    lv_draw_rect_dsc_t glow;
    lv_draw_rect_dsc_init(&glow);
    glow.bg_color = SWEEP_COLOR;
    const float step = TRAIL_DEG / TRAIL_STEPS;
    lv_point_t tri[3] = {{CX, CY}, polar(sweep_deg, RADAR_R), {0, 0}};
    for (int i = 0; i < TRAIL_STEPS; i++) {
        tri[2] = polar(sweep_deg - step * (i + 1), RADAR_R);
        glow.bg_opa = (lv_opa_t)(90 * (TRAIL_STEPS - i) * (TRAIL_STEPS - i) / (TRAIL_STEPS * TRAIL_STEPS));
        lv_draw_polygon(ctx, &glow, tri, 3);
        tri[1] = tri[2];
    }
    lv_draw_line_dsc_t beam;
    lv_draw_line_dsc_init(&beam);
    beam.color = SWEEP_COLOR;
    beam.width = SWEEP_WIDTH;
    beam.round_start = beam.round_end = 1;
    const lv_point_t center = {CX, CY};
    lv_draw_line(ctx, &beam, &center, &sweep_tip);
}
/* Redraw the wedge from the old trail's end to the new beam, one radial band at a time
   (small boxes instead of one box around the whole wedge). */
static void invalidate_sweep(float from_deg, float to_deg) {
    for (int k = 1; k <= SWEEP_SEGS; k++) {
        float r0 = (float)RADAR_R * (k - 1) / SWEEP_SEGS, r1 = (float)RADAR_R * k / SWEEP_SEGS;
        lv_area_t box = {LV_COORD_MAX, LV_COORD_MAX, LV_COORD_MIN, LV_COORD_MIN};
        for (float d = from_deg;; d += 5.f) {
            if (d > to_deg) d = to_deg;
            lv_point_t p[2] = {polar(d, r0), polar(d, r1)};
            for (int j = 0; j < 2; j++) {
                box.x1 = LV_MIN(box.x1, p[j].x); box.y1 = LV_MIN(box.y1, p[j].y);
                box.x2 = LV_MAX(box.x2, p[j].x); box.y2 = LV_MAX(box.y2, p[j].y);
            }
            if (d >= to_deg) break;
        }
        box.x1 -= SWEEP_WIDTH + 1; box.y1 -= SWEEP_WIDTH + 1;
        box.x2 += SWEEP_WIDTH + 1; box.y2 += SWEEP_WIDTH + 1;
        lv_obj_invalidate_area(sweep, &box);
    }
}
static void sweep_tick(lv_timer_t *timer) {
    (void)timer;
    float deg;
    if (frame_clock) {
        static uint32_t phase;           /* frames into the current turn; unaffected by counter wrap */
        uint32_t v = frame_clock(), step = v - sweep_frame;
        if (step < SWEEP_FRAMES_PER_STEP) return;
        sweep_frame = v;
        phase = (uint32_t)((phase + (uint64_t)step) % SWEEP_FRAMES_PER_TURN);
        deg = (float)phase * (360.f / SWEEP_FRAMES_PER_TURN);
    } else deg = (float)(lv_tick_get() % SWEEP_MS_PER_TURN) * (360.f / SWEEP_MS_PER_TURN);
    lv_point_t tip = polar(deg, RADAR_R);
    if (tip.x == sweep_tip.x && tip.y == sweep_tip.y) return;
    float prev = sweep_deg;
    if (deg < prev) prev -= 360.f;   /* wrapped past north */
    if (deg - prev <= 45.f) invalidate_sweep(prev - TRAIL_DEG - 1.f, deg + 1.f);
    else {                           /* resumed after a pause: old and new wedge separately */
        invalidate_sweep(sweep_deg - TRAIL_DEG - 1.f, sweep_deg + 1.f);
        invalidate_sweep(deg - TRAIL_DEG - 1.f, deg + 1.f);
    }
    sweep_deg = deg;
    sweep_tip = tip;
    if (frame_clock) lv_refr_now(NULL);   /* render now, so it shows on the next refresh */
}
void skyframe_ui_set_frame_clock(uint32_t (*clock)(void)) { frame_clock = clock; }
/* The sweep only runs while the radar itself is on screen. */
static void sweep_run(bool run) {
    if (!sweep_timer) return;
    if (run) lv_timer_resume(sweep_timer);
    else lv_timer_pause(sweep_timer);
}

/* The scope: rings and crosshair drawn once into a full-screen canvas, over the plain
   background or (satellite view) a darkened picture of the area. */
static lv_obj_t *scope;
static uint16_t *sat_px;          /* 480x480, PSRAM; owned here */
static bool sat_on;
static void draw_scope(void) {
    if (!scope) return;
    static lv_point_t vertical[] = {{240,36},{240,444}};
    static lv_point_t horizontal[] = {{36,240},{444,240}};
    bool sat = sat_on && sat_px;
    if (sat) {
        /* Darkened so planes, labels and the sweep stand out; darker still outside the scope. */
        lv_color_t *buf = (lv_color_t *)lv_canvas_get_img(scope)->data;
        for (int y = 0; y < SIZE; y++) {
            int dy = y - CY;
            for (int x = 0; x < SIZE; x++) {
                int dx = x - CX;
                lv_color_t c; c.full = sat_px[y * SIZE + x];
                buf[y * SIZE + x] = lv_color_mix(c, BG, dx * dx + dy * dy <= RADAR_R * RADAR_R ? 150 : 70);
            }
        }
    } else lv_canvas_fill_bg(scope, BG, LV_OPA_COVER);
    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = sat ? GREEN : DIM_GREEN;
    ring.opa = sat ? LV_OPA_50 : LV_OPA_COVER;
    ring.width = 1;
    for (int r = 1; r <= 4; r++) lv_canvas_draw_arc(scope, CX, CY, r * 48, 0, 360, &ring);
    lv_draw_line_dsc_t cross;
    lv_draw_line_dsc_init(&cross);
    cross.color = ring.color;
    cross.opa = ring.opa;
    cross.width = 1;
    lv_canvas_draw_line(scope, vertical, 2, &cross);
    lv_canvas_draw_line(scope, horizontal, 2, &cross);
    lv_obj_invalidate(scope);
}
void skyframe_ui_set_satellite_image(uint16_t *pixels) {
    free(sat_px);
    sat_px = pixels;
    if (sat_on) draw_scope();
}
bool skyframe_ui_has_satellite_image(void) { return sat_px != NULL; }
void skyframe_ui_show_satellite(bool on) {
    if (on == sat_on) return;
    sat_on = on;
    if (sat_px) draw_scope();
}
void skyframe_ui_init(lv_obj_t *parent) {
    /* No round mask on full-screen pages: the glass is already round, and a circular
       radius makes every redraw go through a mask and repaint what is underneath. */
    root = lv_obj_create(parent);
    lv_obj_set_size(root, SIZE, SIZE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    sf_gesture_attach(root);   /* long press: next screen; 5 s hold: settings */

    /* Rings and crosshair never move: draw them once into a full-screen image, so every
       sweep frame copies pixels instead of recomputing four circles in each redrawn box.
       Falls back to live objects if the 450 KB (PSRAM) buffer isn't available. */
    static lv_point_t vertical[] = {{240,36},{240,444}};
    static lv_point_t horizontal[] = {{36,240},{444,240}};
    void *scope_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(SIZE, SIZE), MALLOC_CAP_SPIRAM);
    if (scope_buf) {
        scope = lv_canvas_create(root);
        lv_obj_clear_flag(scope, LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_buffer(scope, scope_buf, SIZE, SIZE, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(scope, 0, 0);
        draw_scope();
    } else {   /* no satellite view without the canvas */
        for (int r = 1; r <= 4; r++) {
            lv_obj_t *circle = lv_obj_create(root);
            int dia = r * 96;
            lv_obj_set_size(circle, dia, dia);
            lv_obj_center(circle);
            lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(circle, DIM_GREEN, 0);
            lv_obj_set_style_border_width(circle, 1, 0);
            lv_obj_set_style_pad_all(circle, 0, 0);
            lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
        }
        make_line(root, vertical, DIM_GREEN, 1);
        make_line(root, horizontal, DIM_GREEN, 1);
    }
    static const struct { const char *t; lv_align_t a; int dx, dy; } compass_pts[] = {
        {"N", LV_ALIGN_TOP_MID, 0, 12}, {"E", LV_ALIGN_RIGHT_MID, -12, 0},
        {"S", LV_ALIGN_BOTTOM_MID, 0, -12}, {"W", LV_ALIGN_LEFT_MID, 12, 0}};
    for (size_t k = 0; k < 4; k++) {
        lv_obj_t *l = new_label(root, compass_pts[k].t, 0, 0, GREEN, LV_FONT_DEFAULT);
        lv_obj_align(l, compass_pts[k].a, compass_pts[k].dx, compass_pts[k].dy);
    }
    sweep = lv_obj_create(root);
    plain_box(sweep);                    /* transparent, and touches pass through */
    lv_obj_set_size(sweep, SIZE, SIZE);
    lv_obj_set_pos(sweep, 0, 0);
    lv_obj_add_event_cb(sweep, sweep_draw, LV_EVENT_DRAW_MAIN, NULL);

    for (uint8_t i = 0; i < SKYFRAME_MAX_AIRCRAFT; i++) {
        plane_labels[i] = new_label(root, "", 0, 0, GREEN, PLANE_LABEL_FONT);
        lv_obj_set_size(plane_labels[i], PLANE_LABEL_W, PLANE_LABEL_H);
        lv_label_set_long_mode(plane_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(plane_labels[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(plane_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* Plane glyphs, which are also the tap targets, above the labels. Created farthest-first
       so where two overlap the nearer plane (lower index) is on top and gets the tap. */
    for (int i = SKYFRAME_MAX_AIRCRAFT - 1; i >= 0; i--) {
        planes[i] = lv_obj_create(root);
        lv_obj_remove_style_all(planes[i]);   /* transparent: only plane_draw paints it */
        lv_obj_set_size(planes[i], PLANE_BOX, PLANE_BOX);
        lv_obj_clear_flag(planes[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(planes[i], LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(planes[i], plane_draw, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(planes[i], open_details, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        sf_gesture_track_press(planes[i]);   /* a swipe down may start on a plane */
    }

    /* Static airport marker is drawn above moving aircraft, below details. */
    airport_pin = lv_obj_create(root);
    lv_obj_remove_style_all(airport_pin);
    lv_obj_set_size(airport_pin, AIRPORT_PIN, AIRPORT_PIN);
    lv_obj_clear_flag(airport_pin, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(airport_pin, airport_pin_draw, LV_EVENT_DRAW_MAIN, NULL);
    airport_text = new_label(root, "BWI", 0, 0, lv_color_hex(0xFFD184), LV_FONT_DEFAULT);
    lv_label_set_recolor(airport_text, true);   /* the count is in a quieter color */
    lv_obj_clear_flag(airport_text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(airport_pin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(airport_text, LV_OBJ_FLAG_HIDDEN);

    /* Feed status (waiting / unavailable) so an empty scope never reads as clear sky. */
    /* Clock between the outer two rings at the top (empty until the time is known). */
    /* Clock and status sit on small dark pills so the crosshair and sweep don't cut
       through the text; both are centered and sized to their text. */
    clock_label = new_label(root, "", 0, 0, GREEN, &lv_font_montserrat_16);
    status_label = new_label(root, "", 0, 0, lv_color_hex(0xFFD184), LV_FONT_DEFAULT);
    /* Radar center: latitude and longitude on either side of the vertical crosshair. */
    coords_lat = new_label(root, "", 0, 0, DIM_TEXT, &lv_font_montserrat_12);
    coords_lon = new_label(root, "", 0, 0, DIM_TEXT, &lv_font_montserrat_12);
    range_label = new_label(root, "", 0, 0, DIM_TEXT, &lv_font_montserrat_12);   /* scope radius */
    lv_obj_t *pills[5] = {clock_label, status_label, coords_lat, coords_lon, range_label};
    for (int k = 0; k < 5; k++) {
        lv_obj_set_style_bg_color(pills[k], BG, 0);
        lv_obj_set_style_bg_opa(pills[k], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pills[k], 6, 0);
        lv_obj_set_style_pad_ver(pills[k], 2, 0);
        lv_obj_set_style_radius(pills[k], 8, 0);
        lv_obj_clear_flag(pills[k], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, CLOCK_TOP_Y);       /* or swapped: place_clock */
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, STATUS_BOTTOM_Y);
    /* Across the outer ring, above "S"; the crosshair shows in the gap between them. */
    lv_obj_align(coords_lat, LV_ALIGN_TOP_RIGHT, -(SIZE - CX + 5), 424);
    lv_obj_align(coords_lon, LV_ALIGN_TOP_LEFT, CX + 5, 424);
    /* Near the top, left of the "N", between the outer ring and the edge of the glass. */
    lv_obj_align(range_label, LV_ALIGN_TOP_MID, -64, 26);
    /* Wi-Fi signal: mirrors the radius label, right of the "N". Four bars of rising height
       on a small dark pill like the other labels. */
    wifi_icon = lv_obj_create(root);
    plain_box(wifi_icon);
    lv_obj_set_size(wifi_icon, 34, 19);
    lv_obj_set_style_bg_color(wifi_icon, BG, 0);
    lv_obj_set_style_bg_opa(wifi_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_icon, 8, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_MID, 64, 26);
    for (int k = 0; k < WIFI_BARS; k++) {
        wifi_bar[k] = lv_obj_create(wifi_icon);
        plain_box(wifi_bar[k]);
        int h = 4 + k * 3;
        lv_obj_set_size(wifi_bar[k], 4, h);
        lv_obj_set_pos(wifi_bar[k], 6 + k * 6, 16 - h);
        lv_obj_set_style_radius(wifi_bar[k], 1, 0);
        lv_obj_set_style_bg_opa(wifi_bar[k], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(wifi_bar[k], DIM_GREEN, 0);
    }
    skyframe_ui_set_wifi(-1);
    skyframe_ui_set_radius(display_radius);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);

    build_details(root);

    sweep_timer = lv_timer_create(sweep_tick, SWEEP_PERIOD_MS, NULL);
}

bool skyframe_ui_details_open(void) {
    return detail_screen && !lv_obj_has_flag(detail_screen,LV_OBJ_FLAG_HIDDEN);
}
void skyframe_ui_close_details(void) {
    if (!skyframe_ui_details_open()) return;
    lv_obj_add_flag(detail_screen, LV_OBJ_FLAG_HIDDEN);
    redraw_planes();                     /* planes were not redrawn while it was open */
    if (!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) sweep_run(true);
}
void skyframe_ui_set_status(const char *text) {
    static const char *shown;
    if (!status_label || text == shown) return;
    shown = text;
    if (text) {
        lv_label_set_text_static(status_label, text);
        lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}
void skyframe_ui_show(bool show){
    if (!root)return;
    if(show){
        lv_obj_clear_flag(root,LV_OBJ_FLAG_HIDDEN);
        sweep_run(!skyframe_ui_details_open());
    }else{
        /* Leaving the radar closes any details page, so it can't reappear later. */
        skyframe_ui_close_details();
        lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);
        sweep_run(false);
    }
}

void skyframe_ui_set_select_cb(void (*cb)(const skyframe_aircraft_t *aircraft)){on_selected=cb;}
void skyframe_ui_set_radius(float miles){
    if(miles<1.f||miles>100.f)return;
    display_radius=miles;
    if(isfinite(airport_dist))skyframe_ui_set_airport(airport_dist,airport_bearing,airport_wanted);   /* in or out of range now */
    if(range_label){
        char buf[16];
        snprintf(buf,sizeof(buf),"%.0f mi",miles);
        set_text_if_changed(range_label,buf);
    }
    if(root)redraw_planes();
}

static bool is_selected(const char *icao) {
    return skyframe_ui_details_open() && icao && !strcasecmp(icao, selected_icao);
}
void skyframe_ui_update_info(const sf_aircraft_info_t *info) {
    if (!info || !is_selected(info->icao)) return;
    char buf[96];
    if (info->aircraft_found) {
        /* "Boeing" + "767 424ER"; avoid "Boeing Boeing 767" when the model repeats it. */
        size_t m = strlen(info->manufacturer);
        if (m && strncasecmp(info->model, info->manufacturer, m))
            snprintf(buf, sizeof(buf), "%s %s", info->manufacturer, info->model);
        else snprintf(buf, sizeof(buf), "%s", info->model[0] ? info->model : info->manufacturer);
        if (buf[0]) { lv_label_set_text(d_model, buf); set_item(IT_MODEL, buf); }
        if (info->type_code[0]) set_item(IT_TYPE, info->type_code);
        if (info->registration[0]) set_item(IT_REG, info->registration);
        if (info->owner[0]) set_item(IT_OPERATOR, info->owner);
        set_item(IT_COUNTRY, info->owner_country);
    }
    const char *airline = info->airline[0] ? info->airline : info->owner;
    if (airline[0]) lv_label_set_text(d_airline, airline);
    if (info->route_found) {
        lv_label_set_text(d_orig, info->origin.code);
        lv_label_set_text(d_dest, info->destination.code);
        lv_label_set_text(d_orig_city, info->origin.city);
        lv_label_set_text(d_dest_city, info->destination.city);
        show_route(NULL);
        set_item(IT_FROM, info->origin.name);
        set_item(IT_TO, info->destination.name);
    } else show_route(info->lookup_ok ? "Route not available" : "Route lookup failed");
    if (info->aircraft_found || info->route_found) lv_obj_add_flag(d_lookup, LV_OBJ_FLAG_HIDDEN);
    else lv_label_set_text(d_lookup, info->lookup_ok ? "No database record for this aircraft"
                                                     : "Aircraft record unavailable right now");
    if (!info->photo_url[0] && !photo_shown())
        lv_label_set_text(d_photo_msg, "No photo available");
}
bool skyframe_ui_set_photo(const char *icao, uint16_t *pixels) {
    if (!pixels || !is_selected(icao)) return false;
    fade_finish();
    /* The new photo goes on the free layer, above the one showing, and fades in. */
    int in = d_front < 0 ? 0 : 1 - d_front;
    drop_layer(in);
    d_pixels[in] = pixels;
    lv_img_dsc_t *img = &d_img[in];
    memset(img, 0, sizeof(*img));
    img->header.cf = LV_IMG_CF_TRUE_COLOR;
    img->header.w = SKYFRAME_PHOTO_W;
    img->header.h = SKYFRAME_PHOTO_H;
    img->data_size = SKYFRAME_PHOTO_W * SKYFRAME_PHOTO_H * 2;
    img->data = (const uint8_t *)pixels;
    lv_img_set_src(d_photo[in], img);
    if (d_front >= 0 && lv_obj_get_index(d_photo[in]) < lv_obj_get_index(d_photo[d_front]))
        lv_obj_move_to_index(d_photo[in], (int32_t)lv_obj_get_index(d_photo[d_front]));
    lv_obj_set_style_img_opa(d_photo[in], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(d_photo[in], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d_photo_msg, LV_OBJ_FLAG_HIDDEN);
    d_fading = in;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, d_photo[in]); lv_anim_set_exec_cb(&a, fade_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER); lv_anim_set_time(&a, PHOTO_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out); lv_anim_set_ready_cb(&a, fade_ready);
    lv_anim_start(&a);
    return true;
}
void skyframe_ui_photo_unavailable(const char *icao) {
    if (is_selected(icao) && !photo_shown())
        lv_label_set_text(d_photo_msg, "No photo available");
}

void skyframe_ui_set_clock(const char *text){
    if(clock_label&&strcmp(lv_label_get_text(clock_label),text?text:""))lv_label_set_text(clock_label,text?text:"");
}
void skyframe_ui_set_location(double latitude, double longitude){
    if(!coords_lat||!coords_lon)return;
    char buf[24];   /* "39.1500° N" | "76.7000° W" */
    snprintf(buf,sizeof(buf),"%.4f" DEG " %c",fabs(latitude),latitude<0?'S':'N');
    set_text_if_changed(coords_lat,buf);
    snprintf(buf,sizeof(buf),"%.4f" DEG " %c",fabs(longitude),longitude<0?'W':'E');
    set_text_if_changed(coords_lon,buf);
}
/* bars: 0-4 signal bars, or -1 when not connected (all bars dim, the first amber). */
void skyframe_ui_set_wifi(int bars){
    static int shown = -2;
    if(!wifi_icon || bars == shown) return;
    shown = bars;
    for(int k = 0; k < WIFI_BARS; k++){
        lv_color_t c = k < bars ? GREEN : DIM_GREEN;
        if(bars < 0 && k == 0) c = lv_color_hex(0xFFD184);
        lv_obj_set_style_bg_color(wifi_bar[k], c, 0);
    }
}