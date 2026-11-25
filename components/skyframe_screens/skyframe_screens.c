#include "skyframe_lvmem.h"
#include "skyframe_screens.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout tuned for a 480x480 ROUND LCD: every row is kept inside the circle
   at its height (see the y coordinates in sf_screens_init). */
#define NAVY lv_color_hex(0x101D35)
#define WHITE lv_color_hex(0xF5F9FF)
#define MUTED lv_color_hex(0xB4C3D9)
#define FAINT lv_color_hex(0x7F91AD)
#define DEG "\xC2\xB0"   /* UTF-8 degree sign; included in LVGL's Montserrat fonts */
/* Weather page background: a gentle vertical gradient, bluer by day, inkier at night. */
#define DAY_TOP    lv_color_hex(0x1F3F73)
#define DAY_BOT    lv_color_hex(0x0E1D3B)
#define NIGHT_TOP  lv_color_hex(0x161D3D)
#define NIGHT_BOT  lv_color_hex(0x070A16)
static lv_obj_t *weather_root, *photos_root, *temp_label, *condition_label;
static lv_obj_t *feels_label, *clock_label, *footer_label;
static lv_obj_t *screens_parent;     /* for the settings and test screens, built on demand */
static lv_obj_t *range_low, *range_high, *range_bar, *range_dot;
enum { CHIP_HUMIDITY, CHIP_WIND, CHIP_RAIN, CHIP_SUN, CHIP_COUNT };
static lv_obj_t *chip_value[CHIP_COUNT], *sun_chip_name;
static lv_color_t page_top, page_bot;   /* set by set_page_colors() */
static void build_usb_notice(lv_obj_t *parent);
static void build_verse(lv_obj_t *parent);
static void build_birthday(lv_obj_t *parent);
static void photo_swiped(lv_event_t *e);
static lv_obj_t *text_label(lv_obj_t *parent,const char *s,lv_color_t c,const lv_font_t *font);
static void build_today(lv_obj_t *parent);
static void alert_pulse_run(void);
static void (*on_birthday_dismiss)(void);
static void (*on_long_press)(void), (*on_hold)(void), (*on_setup_close)(void);
/* Touch gestures on the radar, weather and photo screens:
   long press, released before 5 s  -> next screen of the rotation
   hold 5 s                         -> settings screen (released afterwards: nothing else) */
#define HOLD_FOR_SETTINGS_MS 5000
#define QUICK_SWIPE_TOP 160          /* a swipe down that starts above this opens the panel */
static uint32_t pressed_at;
static lv_coord_t pressed_y;
static bool long_seen, hold_fired, swiped;   /* swiped: a slow swipe is not a long press */
static void quick_open(void);
static void gesture(lv_event_t *e){
    switch(lv_event_get_code(e)){
    case LV_EVENT_PRESSED: {
        pressed_at=lv_tick_get(); long_seen=hold_fired=swiped=false;
        lv_point_t p={0,0};
        if(lv_indev_get_act())lv_indev_get_point(lv_indev_get_act(),&p);
        pressed_y=p.y;
        break;
    }
    case LV_EVENT_GESTURE:
        swiped=true;   /* any swipe: not a long press (a slow one would otherwise skip the screen) */
        if(lv_indev_get_gesture_dir(lv_indev_get_act())==LV_DIR_BOTTOM&&pressed_y<QUICK_SWIPE_TOP){
            /* The rest of this touch belongs to nobody: no click on whatever it started on
               (a plane on the radar would open its details behind the panel). */
            lv_indev_wait_release(lv_indev_get_act());
            quick_open();
        }
        break;
    case LV_EVENT_LONG_PRESSED: long_seen=true; break;
    case LV_EVENT_PRESSING:
        if(!hold_fired&&!swiped&&lv_tick_elaps(pressed_at)>=HOLD_FOR_SETTINGS_MS){hold_fired=true;if(on_hold)on_hold();}
        break;
    case LV_EVENT_RELEASED: if(long_seen&&!hold_fired&&!swiped&&on_long_press)on_long_press(); break;
    default: break;
    }
}
/* Children that keep their own presses (radar planes): note where the finger went down, so a
   swipe that starts on them is judged from the right place. */
static void press_point(lv_event_t *e){
    (void)e;
    lv_point_t p={0,0};
    if(lv_indev_get_act())lv_indev_get_point(lv_indev_get_act(),&p);
    pressed_y=p.y;pressed_at=lv_tick_get();long_seen=hold_fired=swiped=false;
}
void sf_gesture_track_press(lv_obj_t *obj){lv_obj_add_event_cb(obj,press_point,LV_EVENT_PRESSED,NULL);}
void sf_gesture_attach(lv_obj_t *obj){
    static const lv_event_code_t codes[]={LV_EVENT_PRESSED,LV_EVENT_LONG_PRESSED,LV_EVENT_PRESSING,LV_EVENT_RELEASED,LV_EVENT_GESTURE};
    for(size_t i=0;i<sizeof(codes)/sizeof(codes[0]);i++)lv_obj_add_event_cb(obj,gesture,codes[i],NULL);
    /* LVGL hands a swipe to the first object up the tree that doesn't pass it on; without
       this the page passes it to the screen and never sees it (only the photo page, which
       keeps its own for page turns, did). */
    lv_obj_clear_flag(obj,LV_OBJ_FLAG_GESTURE_BUBBLE);
}
static lv_obj_t *forecast_time[SF_WEATHER_HOURS], *forecast_temp[SF_WEATHER_HOURS];
/* Each photo sits in its own full-screen frame; transitions move, resize or clip the
   frame (the image inside stays put), or fade the incoming image over the current one. */
static lv_obj_t *frame_a, *frame_b, *picture_a, *picture_b, *no_photos_label;
static lv_obj_t *page_flap;          /* back of the page during a page turn */
static lv_dir_t swipe_turn;          /* swipe waiting for the next photo (LV_DIR_LEFT/RIGHT) */
static bool picture_a_active = true, photo_shown, show_instant;
typedef enum { TR_FADE, TR_SLIDE, TR_IRIS, TR_WIPE, TR_PUSH_UP, TR_PUSH_RIGHT, TR_COVER_DOWN,
               TR_DOORS, TR_BLINDS } transition_t;
/* Slideshow order: similar transitions are kept apart. */
static const transition_t transition_order[] = {TR_FADE, TR_PUSH_UP, TR_IRIS, TR_DOORS, TR_SLIDE,
                                               TR_BLINDS, TR_WIPE, TR_COVER_DOWN, TR_PUSH_RIGHT};
#define BLINDS 8                     /* vertical blinds, 60 px each */
static lv_obj_t *blind[BLINDS], *blind_img[BLINDS];
static unsigned next_transition;
static sf_next_photo_cb_t next_photo;
static void *next_photo_ctx;
static uint32_t last_photo_ms;
static bool have_photo_time;

static lv_obj_t *plain_label(lv_obj_t *parent, const char *s, lv_color_t c,
                             const lv_font_t *font, int x, int y, int width) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, s);
    lv_obj_set_width(o, width);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(o, c, 0);
    if (font) lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_pos(o, x, y);
    return o;
}
static lv_obj_t *screen(lv_obj_t *parent, lv_color_t c) {
    lv_obj_t *o=lv_obj_create(parent);
    lv_obj_set_size(o,480,480); lv_obj_set_pos(o,0,0);
    /* No round radius: the glass is already round, and a 480 px circular mask would make
       every redraw slower. */
    lv_obj_set_style_bg_color(o,c,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
    return o;
}
/* WMO weather codes as used by Open-Meteo. */
static const char *description(int code) {
    if (code == 0) return "Clear skies";
    if (code == 1) return "Mostly clear";
    if (code == 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Foggy";
    if (code >= 51 && code <= 55) return "Drizzle";
    if (code == 56 || code == 57) return "Freezing drizzle";
    if (code >= 61 && code <= 65) return "Rain";
    if (code == 66 || code == 67) return "Freezing rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Rain showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code == 95) return "Thunderstorms";
    if (code == 96 || code == 99) return "Thunderstorms, hail";
    return "Cloudy";
}

/* ---- Vector weather icons ------------------------------------------------
   Drawn from LVGL primitives (circles, rounded bars, lines) on a unit grid so
   one design scales from the 76 px current icon to the 40 px forecast icons.
   The moon's crescent is cut with a disc of the page color behind the icon (ic->bg). */
typedef enum { WX_NONE, WX_CLEAR, WX_PARTLY, WX_OVERCAST, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;
#define WX_SUN      lv_color_hex(0xFFC857)
#define WX_MOON     lv_color_hex(0xE6ECF5)
#define WX_CLOUD    lv_color_hex(0xDDE6F2)
#define WX_CLOUD_LO lv_color_hex(0x8E9DB5)
#define WX_RAIN_C   lv_color_hex(0x6EB6FF)
#define WX_BOLT     lv_color_hex(0xFFD15C)
#define WX_FOG_C    lv_color_hex(0x9FB0C8)
#define WX_MAX_PTS  24
typedef struct { lv_obj_t *box; int size; int used; lv_color_t bg; lv_point_t pts[WX_MAX_PTS];
                 int drawn_code; bool drawn_day, drawn; lv_color_t drawn_bg; } wx_icon_t;
static wx_icon_t icon_now, icon_hour[SF_WEATHER_HOURS];

static wx_kind_t wx_kind(int code) {
    if (code < 0) return WX_NONE;
    if (code <= 2) return code == 0 ? WX_CLEAR : WX_PARTLY;
    if (code == 3) return WX_OVERCAST;
    if (code == 45 || code == 48) return WX_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_STORM;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
    return WX_OVERCAST;
}
static int px(const wx_icon_t *ic, float f) { return (int)lroundf(f * ic->size); }
static void wx_plain(lv_obj_t *o) {
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
static void wx_disc(wx_icon_t *ic, float cx, float cy, float r, lv_color_t c) {
    lv_obj_t *o = lv_obj_create(ic->box); wx_plain(o);
    lv_obj_set_size(o, px(ic, 2*r), px(ic, 2*r));
    lv_obj_set_pos(o, px(ic, cx - r), px(ic, cy - r));
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}
static void wx_bar(wx_icon_t *ic, float x1, float y1, float x2, float y2, lv_color_t c) {
    lv_obj_t *o = lv_obj_create(ic->box); wx_plain(o);
    lv_obj_set_pos(o, px(ic, x1), px(ic, y1));
    lv_obj_set_size(o, px(ic, x2 - x1), px(ic, y2 - y1));
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}
/* Polyline through n (x,y) unit-grid points; points live in the icon's own buffer. */
static void wx_stroke(wx_icon_t *ic, const float *xy, int n, lv_color_t c) {
    if (ic->used + n > WX_MAX_PTS) return;
    lv_point_t *p = &ic->pts[ic->used]; ic->used += n;
    for (int i = 0; i < n; i++) { p[i].x = px(ic, xy[2*i]); p[i].y = px(ic, xy[2*i+1]); }
    lv_obj_t *l = lv_line_create(ic->box);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(l, p, n);
    int w = px(ic, 0.07f); if (w < 2) w = 2;
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, c, 0);
}
static void wx_sun(wx_icon_t *ic, float cx, float cy, float k) {
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f, s = sinf(a), c = cosf(a);
        float ray[4] = {cx + s*0.29f*k, cy - c*0.29f*k, cx + s*0.41f*k, cy - c*0.41f*k};
        wx_stroke(ic, ray, 2, WX_SUN);
    }
    wx_disc(ic, cx, cy, 0.19f*k, WX_SUN);
}
static void wx_moon(wx_icon_t *ic, float cx, float cy, float k) {
    wx_disc(ic, cx, cy, 0.26f*k, WX_MOON);
    wx_disc(ic, cx + 0.13f*k, cy - 0.09f*k, 0.22f*k, ic->bg);
}
/* Cloud occupying (ox,oy)..(ox+0.9k, oy+0.72k): flat base plus three puffs. */
static void wx_cloud(wx_icon_t *ic, float ox, float oy, float k, lv_color_t c) {
    wx_disc(ic, ox + 0.33f*k, oy + 0.46f*k, 0.20f*k, c);
    wx_disc(ic, ox + 0.57f*k, oy + 0.38f*k, 0.25f*k, c);
    wx_disc(ic, ox + 0.76f*k, oy + 0.54f*k, 0.16f*k, c);
    wx_bar(ic, ox + 0.10f*k, oy + 0.44f*k, ox + 0.92f*k, oy + 0.72f*k, c);
}
static void wx_draw(wx_icon_t *ic, int code, bool day) {
    if (!ic->box) return;
    lv_obj_clean(ic->box); ic->used = 0;
    switch (wx_kind(code)) {
    case WX_CLEAR:
        if (day) wx_sun(ic, 0.5f, 0.5f, 1.15f); else wx_moon(ic, 0.5f, 0.5f, 1.35f);
        break;
    case WX_PARTLY:
        if (day) wx_sun(ic, 0.36f, 0.36f, 0.85f); else wx_moon(ic, 0.38f, 0.36f, 0.95f);
        wx_cloud(ic, 0.10f, 0.20f, 0.92f, WX_CLOUD);
        break;
    case WX_OVERCAST:
        wx_cloud(ic, 0.22f, 0.02f, 0.78f, WX_CLOUD_LO);
        wx_cloud(ic, 0.02f, 0.20f, 0.92f, WX_CLOUD);
        break;
    case WX_FOG:
        wx_bar(ic, 0.14f, 0.30f, 0.86f, 0.38f, WX_FOG_C);
        wx_bar(ic, 0.06f, 0.46f, 0.78f, 0.54f, WX_FOG_C);
        wx_bar(ic, 0.22f, 0.62f, 0.94f, 0.70f, WX_FOG_C);
        break;
    case WX_RAIN: {
        wx_cloud(ic, 0.04f, -0.06f, 1.0f, WX_CLOUD);
        for (int i = 0; i < 3; i++) {
            float x = 0.34f + i*0.18f, drop[4] = {x, 0.76f, x - 0.07f, 0.94f};
            wx_stroke(ic, drop, 2, WX_RAIN_C);
        }
        break; }
    case WX_SNOW:
        wx_cloud(ic, 0.04f, -0.06f, 1.0f, WX_CLOUD);
        wx_disc(ic, 0.34f, 0.82f, 0.055f, WX_MOON);
        wx_disc(ic, 0.52f, 0.92f, 0.055f, WX_MOON);
        wx_disc(ic, 0.70f, 0.82f, 0.055f, WX_MOON);
        break;
    case WX_STORM: {
        wx_cloud(ic, 0.04f, -0.06f, 1.0f, WX_CLOUD_LO);
        static const float bolt[8] = {0.56f, 0.62f, 0.44f, 0.80f, 0.56f, 0.80f, 0.46f, 0.98f};
        wx_stroke(ic, bolt, 4, WX_BOLT);
        break; }
    default: break;
    }
}
static void wx_icon_init(wx_icon_t *ic, lv_obj_t *parent, int x, int y, int size) {
    ic->box = lv_obj_create(parent); wx_plain(ic->box);
    ic->size = size; ic->used = 0; ic->bg = NAVY;
    lv_obj_set_size(ic->box, size, size);
    lv_obj_set_pos(ic->box, x, y);
}

/* ---- Weather page -------------------------------------------------------- */
/* Page color at a given y, matching the vertical gradient (for the moon's cut-out). */
static lv_color_t page_color_at(int y) {
    if (y < 0) y = 0;
    if (y > 480) y = 480;
    return lv_color_mix(page_bot, page_top, (uint8_t)(y * 255 / 480));
}
static void draw_icon(wx_icon_t *ic, int code, bool day) {
    lv_obj_update_layout(weather_root);   /* flex-placed icons need their final position */
    int y = lv_obj_get_y(ic->box);
    for (lv_obj_t *p = lv_obj_get_parent(ic->box); p && p != weather_root; p = lv_obj_get_parent(p))
        y += lv_obj_get_y(p);
    ic->bg = page_color_at(y + ic->size / 2);
    /* Unchanged since last time: keep its objects (rebuilding churns internal RAM). */
    if (ic->drawn && ic->drawn_code == code && ic->drawn_day == day && lv_color_to32(ic->drawn_bg) == lv_color_to32(ic->bg)) return;
    ic->drawn = true; ic->drawn_code = code; ic->drawn_day = day; ic->drawn_bg = ic->bg;
    wx_draw(ic, code, day);
}
static void plain_obj(lv_obj_t *o) {
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
/* ---- Small drawn icons (the fonts have no radar, sun, plane or raindrop) ----------
   An object of the icon's size that draws itself; not clickable, so taps reach its parent. */
typedef enum { ICON_RADAR, ICON_SUN, ICON_PLANE, ICON_DROP } icon_kind_t;
static lv_opa_t icon_opa = LV_OPA_COVER;
static void icon_line(lv_draw_ctx_t *ctx, lv_color_t c, int w, float x1, float y1, float x2, float y2) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = c; d.width = (lv_coord_t)w; d.round_start = d.round_end = 1; d.opa = icon_opa;
    lv_point_t a = {(lv_coord_t)lroundf(x1), (lv_coord_t)lroundf(y1)}, b = {(lv_coord_t)lroundf(x2), (lv_coord_t)lroundf(y2)};
    lv_draw_line(ctx, &d, &a, &b);
}
static void icon_disc(lv_draw_ctx_t *ctx, lv_color_t c, float cx, float cy, float r) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.radius = LV_RADIUS_CIRCLE; d.bg_opa = icon_opa;
    lv_area_t a = {(lv_coord_t)lroundf(cx - r), (lv_coord_t)lroundf(cy - r), (lv_coord_t)lroundf(cx + r), (lv_coord_t)lroundf(cy + r)};
    lv_draw_rect(ctx, &d, &a);
}
static void icon_ring(lv_draw_ctx_t *ctx, lv_color_t c, int w, float cx, float cy, float r) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = c; d.width = (lv_coord_t)w; d.opa = icon_opa;
    lv_point_t p = {(lv_coord_t)lroundf(cx), (lv_coord_t)lroundf(cy)};
    lv_draw_arc(ctx, &d, &p, (uint16_t)lroundf(r), 0, 360);
}
static void icon_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    icon_kind_t kind = (icon_kind_t)(uintptr_t)lv_event_get_user_data(e);
    lv_area_t a; lv_obj_get_coords(o, &a);
    float s = (float)lv_area_get_width(&a), cx = a.x1 + s / 2.f, cy = a.y1 + (float)lv_area_get_height(&a) / 2.f;
    icon_opa = lv_obj_get_style_opa(o, 0);   /* inherited: a dimmed button dims its icon */
    switch (kind) {
    case ICON_RADAR: {   /* two rings, a sweep line and a blip */
        lv_color_t g = lv_color_hex(0x7CF5A3);
        icon_ring(ctx, g, 2, cx, cy, s / 2.f - 1.f);
        icon_ring(ctx, lv_color_mix(g, lv_color_hex(0x0B1426), LV_OPA_50), 2, cx, cy, s / 4.f);
        icon_line(ctx, g, 2, cx, cy, cx + s * 0.33f, cy - s * 0.33f);
        icon_disc(ctx, g, cx, cy, 2.f);
        icon_disc(ctx, g, cx - s * 0.2f, cy + s * 0.12f, 2.f);
        break;
    }
    case ICON_SUN: {     /* disc with eight rays */
        lv_color_t y = lv_color_hex(0xFFC857);
        float r = s * 0.2f;
        icon_disc(ctx, y, cx, cy, r);
        for (int k = 0; k < 8; k++) {
            float ang = (float)k * 3.14159265f / 4.f, dx = cosf(ang), dy = sinf(ang);
            icon_line(ctx, y, 2, cx + dx * r * 1.6f, cy + dy * r * 1.6f, cx + dx * s * 0.46f, cy + dy * s * 0.46f);
        }
        break;
    }
    case ICON_PLANE: {   /* airliner from above, nose up and to the right */
        static const struct { float f1, s1, f2, s2, w; } st[] = {   /* forward/side, in 1/28ths */
            {-11, 0, 12, 0, 3}, {2, 0, -5, -12, 2}, {2, 0, -5, 12, 2}, {-9, 0, -13, -5, 2}, {-9, 0, -13, 5, 2}};
        float k = s / 28.f, fx = 0.7071f, fy = -0.7071f, sx = 0.7071f, sy = 0.7071f;   /* heading 45 degrees */
        lv_color_t w = lv_color_hex(0xF5F9FF);
        for (size_t i = 0; i < sizeof(st) / sizeof(st[0]); i++)
            icon_line(ctx, w, (int)lroundf(st[i].w * k), cx + (st[i].f1 * fx + st[i].s1 * sx) * k, cy + (st[i].f1 * fy + st[i].s1 * sy) * k,
                      cx + (st[i].f2 * fx + st[i].s2 * sx) * k, cy + (st[i].f2 * fy + st[i].s2 * sy) * k);
        break;
    }
    case ICON_DROP: {    /* round bottom narrowing to a point at the top */
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0x6EB6FF); d.bg_opa = icon_opa;
        float r = s * 0.32f, by = cy + s * 0.16f;
        lv_point_t p[14];
        int n = 0;
        p[n++] = (lv_point_t){(lv_coord_t)lroundf(cx), (lv_coord_t)lroundf(cy - s * 0.5f)};
        for (int k = 0; k <= 12; k++) {   /* from 30 degrees above the right, round the bottom */
            float ang = (-30.f + 240.f * (float)k / 12.f) * 3.14159265f / 180.f;
            p[n++] = (lv_point_t){(lv_coord_t)lroundf(cx + cosf(ang) * r), (lv_coord_t)lroundf(by + sinf(ang) * r)};
            if (n >= 14) break;
        }
        lv_draw_polygon(ctx, &d, p, (uint16_t)n);
        break;
    }
    }
}
static lv_obj_t *drawn_icon(lv_obj_t *parent, icon_kind_t kind, int size) {
    lv_obj_t *o = lv_obj_create(parent); plain_obj(o);
    lv_obj_set_size(o, size, size);
    lv_obj_add_event_cb(o, icon_draw, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)kind);
    return o;
}
/* "16:49" -> "4:49 PM"; empty on bad input. */
static void clock_12h(const char *hhmm, char *out, size_t cap) {
    int h = -1, m = -1;
    out[0] = 0;
    if (!hhmm || sscanf(hhmm, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return;
    snprintf(out, cap, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h >= 12 ? "PM" : "AM");
}
static const char *compass8(float deg) {
    static const char *n[8] = {"N","NE","E","SE","S","SW","W","NW"};
    return n[((int)lroundf(fmodf(deg + 360.f, 360.f) / 45.f)) % 8];
}
static void set_page_colors(bool day) {
    page_top = day ? DAY_TOP : NIGHT_TOP;
    page_bot = day ? DAY_BOT : NIGHT_BOT;
    lv_obj_set_style_bg_color(weather_root, page_top, 0);
    lv_obj_set_style_bg_grad_color(weather_root, page_bot, 0);
    lv_obj_set_style_border_color(range_dot, page_color_at(lv_obj_get_y(range_dot)), 0);
}
static void build_weather(lv_obj_t *parent) {
    weather_root = screen(parent, DAY_TOP);
    lv_obj_set_style_bg_grad_color(weather_root, DAY_BOT, 0);
    lv_obj_set_style_bg_grad_dir(weather_root, LV_GRAD_DIR_VER, 0);

    /* y 24-51: clock (empty until the time is known), near the top of the glass */
    clock_label = plain_label(weather_root, "", WHITE, &lv_font_montserrat_24, 140, 24, 200);

    /* y 70-166: icon beside the big temperature, centered as a pair */
    lv_obj_t *hero = lv_obj_create(weather_root); plain_obj(hero);
    lv_obj_set_size(hero, 480, 96);
    lv_obj_set_pos(hero, 0, 70);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 14, 0);
    wx_icon_init(&icon_now, hero, 0, 0, 92);
    temp_label = lv_label_create(hero);
    lv_label_set_text(temp_label, "--" DEG);
    lv_obj_set_style_text_color(temp_label, WHITE, 0);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_48, 0);

    /* y 170-218: condition and feels-like */
    condition_label = plain_label(weather_root, "Waiting for weather", WHITE, &lv_font_montserrat_20, 60, 170, 360);
    feels_label = plain_label(weather_root, "", MUTED, LV_FONT_DEFAULT, 90, 198, 300);

    /* y 226: today's low-high range with a marker for now */
    range_low = plain_label(weather_root, "--", MUTED, &lv_font_montserrat_16, 92, 226, 56);
    lv_obj_set_style_text_align(range_low, LV_TEXT_ALIGN_RIGHT, 0);
    range_high = plain_label(weather_root, "--", MUTED, &lv_font_montserrat_16, 332, 226, 56);
    lv_obj_set_style_text_align(range_high, LV_TEXT_ALIGN_LEFT, 0);
    range_bar = lv_obj_create(weather_root); plain_obj(range_bar);
    lv_obj_set_pos(range_bar, 158, 232);
    lv_obj_set_size(range_bar, 164, 6);
    lv_obj_set_style_radius(range_bar, 3, 0);
    lv_obj_set_style_bg_opa(range_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(range_bar, lv_color_hex(0x5AA9FF), 0);
    lv_obj_set_style_bg_grad_color(range_bar, lv_color_hex(0xFFB547), 0);
    lv_obj_set_style_bg_grad_dir(range_bar, LV_GRAD_DIR_HOR, 0);
    range_dot = lv_obj_create(weather_root); plain_obj(range_dot);
    lv_obj_set_size(range_dot, 14, 14);
    lv_obj_set_style_radius(range_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(range_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(range_dot, WHITE, 0);
    lv_obj_set_style_border_width(range_dot, 3, 0);
    lv_obj_set_pos(range_dot, 233, 228);
    lv_obj_add_flag(range_dot, LV_OBJ_FLAG_HIDDEN);

    /* y 258-312: humidity / wind / rain / next sunrise or sunset chips */
    /* Widths fit what each shows: wind needs the most ("25 mph NW"), rain the least ("100%").
       348 px in all, centered. */
    static const char *chip_names[CHIP_COUNT] = {"HUMIDITY", "WIND", "RAIN", ""};
    static const int chip_w[CHIP_COUNT] = {74, 108, 64, 84};
    for (int i = 0, x = 66; i < CHIP_COUNT; x += chip_w[i] + 6, i++) {
        int w = chip_w[i];
        lv_obj_t *chip = lv_obj_create(weather_root); plain_obj(chip);
        lv_obj_set_pos(chip, x, 258);
        lv_obj_set_size(chip, w, 54);
        lv_obj_set_style_radius(chip, 14, 0);
        lv_obj_set_style_bg_color(chip, WHITE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_10, 0);
        chip_value[i] = plain_label(chip, "--", WHITE, LV_FONT_DEFAULT, 0, 28, w);   /* "100 mph NW" fits in wind's */
        if (i != CHIP_SUN && i != CHIP_RAIN) {
            plain_label(chip, chip_names[i], MUTED, &lv_font_montserrat_12, 0, 8, w);
            continue;
        }
        /* Header with an icon: a raindrop before RAIN, a sun on the horizon before SUNRISE / SUNSET. */
        lv_obj_t *head = lv_obj_create(chip); plain_obj(head);
        lv_obj_set_size(head, w, 16); lv_obj_set_pos(head, 0, 7);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 4, 0);
        if (i == CHIP_RAIN) {
            drawn_icon(head, ICON_DROP, 12);
            text_label(head, "RAIN", MUTED, &lv_font_montserrat_12);
            continue;
        }
        lv_obj_t *icon = lv_obj_create(head); plain_obj(icon);   /* clips the disc to its top half */
        lv_obj_set_size(icon, 16, 10);
        lv_obj_t *disc = lv_obj_create(icon); plain_obj(disc);
        lv_obj_set_size(disc, 12, 12); lv_obj_set_pos(disc, 2, 2);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(disc, WX_SUN, 0); lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
        lv_obj_t *horizon = lv_obj_create(icon); plain_obj(horizon);
        lv_obj_set_size(horizon, 16, 2); lv_obj_set_pos(horizon, 0, 8);
        lv_obj_set_style_bg_color(horizon, WX_SUN, 0); lv_obj_set_style_bg_opa(horizon, LV_OPA_COVER, 0);
        sun_chip_name = text_label(head, "SUNSET", MUTED, &lv_font_montserrat_12);
    }

    /* y 326-402: next hours */
    for (int i = 0; i < SF_WEATHER_HOURS; i++) {
        int x = 94 + i * 74;
        forecast_time[i] = plain_label(weather_root, "--", MUTED, &lv_font_montserrat_12, x, 326, 70);
        wx_icon_init(&icon_hour[i], weather_root, x + 17, 344, 36);
        forecast_temp[i] = plain_label(weather_root, "--", WHITE, &lv_font_montserrat_16, x, 384, 70);
    }

    /* y 414: data age, so stale weather is never mistaken for current */
    footer_label = plain_label(weather_root, "", FAINT, &lv_font_montserrat_12, 100, 414, 280);
    set_page_colors(true);
}
void sf_screens_init(lv_obj_t *parent) {
    /* The display's screen object is scrollable by default. Then a vertical drag on any page
       counts as an attempt to scroll it, and LVGL reports no swipe: the pull-down panel only
       opened from the photo page (whose swipes are sideways). Nothing here ever scrolls it. */
    lv_obj_clear_flag(parent,LV_OBJ_FLAG_SCROLLABLE);
    build_weather(parent);
    photos_root=screen(parent,lv_color_hex(0x000000));
    sf_gesture_attach(weather_root);
    sf_gesture_attach(photos_root);
    build_verse(parent);
    build_birthday(parent);
    build_today(parent);
    screens_parent=parent;
    lv_obj_t **frames[2]={&frame_a,&frame_b}, **pics[2]={&picture_a,&picture_b};
    for(int i=0;i<2;i++){
        *frames[i]=lv_obj_create(photos_root);plain_obj(*frames[i]);
        lv_obj_set_size(*frames[i],480,480);lv_obj_set_pos(*frames[i],0,0);
        *pics[i]=lv_img_create(*frames[i]);lv_obj_set_pos(*pics[i],0,0);
        lv_obj_add_flag(*frames[i],LV_OBJ_FLAG_HIDDEN);
    }
    for(int k=0;k<BLINDS;k++){   /* slats for the blinds transition */
        blind[k]=lv_obj_create(photos_root);plain_obj(blind[k]);
        lv_obj_set_pos(blind[k],(lv_coord_t)(k*480/BLINDS),0);lv_obj_set_size(blind[k],480/BLINDS,480);
        blind_img[k]=lv_img_create(blind[k]);lv_obj_set_pos(blind_img[k],(lv_coord_t)(-k*480/BLINDS),0);
        lv_obj_add_flag(blind[k],LV_OBJ_FLAG_HIDDEN);
    }
    /* Swipe left or right: turn the page to the next photo. Gestures stop here instead
       of bubbling up to the screen. */
    lv_obj_clear_flag(photos_root,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(photos_root,photo_swiped,LV_EVENT_GESTURE,NULL);
    page_flap=lv_obj_create(photos_root);plain_obj(page_flap);
    lv_obj_set_style_bg_opa(page_flap,LV_OPA_COVER,0);
    lv_obj_set_style_bg_grad_dir(page_flap,LV_GRAD_DIR_HOR,0);
    lv_obj_set_style_shadow_width(page_flap,18,0);
    lv_obj_set_style_shadow_opa(page_flap,LV_OPA_60,0);
    lv_obj_set_style_shadow_color(page_flap,lv_color_hex(0x000000),0);
    lv_obj_add_flag(page_flap,LV_OBJ_FLAG_HIDDEN);
    /* Shown only while there is no photo on screen; never over a family photo. */
    no_photos_label=plain_label(photos_root,"Loading photos...",MUTED,LV_FONT_DEFAULT,90,190,300);
    lv_obj_align(no_photos_label,LV_ALIGN_CENTER,0,0);   /* stays centered for 1-4 lines */
    build_usb_notice(parent);   /* last: drawn above every other screen */
}
void sf_weather_show(bool show) {
    if(weather_root){if(show)lv_obj_clear_flag(weather_root,LV_OBJ_FLAG_HIDDEN);
        else {lv_obj_add_flag(weather_root,LV_OBJ_FLAG_HIDDEN);sf_weather_close_alert();}}
    alert_pulse_run();
}
void sf_photo_show(bool show) {
    swipe_turn=LV_DIR_NONE;
    if(photos_root){if(show){lv_obj_clear_flag(photos_root,LV_OBJ_FLAG_HIDDEN); have_photo_time=false; show_instant=true;}
        else lv_obj_add_flag(photos_root,LV_OBJ_FLAG_HIDDEN);}
}
void sf_weather_update(const sf_weather_t *w) {
    if(!temp_label||!w||!w->valid)return;
    char b[96], t[16];
    set_page_colors(w->is_day);
    snprintf(b,sizeof(b),"%.0f" DEG,w->temp_f);lv_label_set_text(temp_label,b);
    lv_label_set_text(condition_label,description(w->weather_code));
    draw_icon(&icon_now,w->weather_code,w->is_day);
    if(isfinite(w->feels_f)) snprintf(b,sizeof(b),"Feels like %.0f" DEG,w->feels_f);
    else b[0]=0;
    lv_label_set_text(feels_label,b);

    /* Low-high bar with today's position */
    snprintf(b,sizeof(b),"%.0f" DEG,w->low_f);lv_label_set_text(range_low,b);
    snprintf(b,sizeof(b),"%.0f" DEG,w->high_f);lv_label_set_text(range_high,b);
    float span=w->high_f-w->low_f, f=span>0.5f?(w->temp_f-w->low_f)/span:0.5f;
    if(f<0)f=0;
    if(f>1)f=1;
    lv_obj_set_x(range_dot,158+(int)lroundf(f*164.f)-7);
    lv_obj_clear_flag(range_dot,LV_OBJ_FLAG_HIDDEN);

    /* Chips: "--" when the service did not supply the value */
    if(w->humidity>=0) snprintf(b,sizeof(b),"%d%%",w->humidity); else snprintf(b,sizeof(b),"--");
    lv_label_set_text(chip_value[CHIP_HUMIDITY],b);
    if(isfinite(w->wind_mph)) {
        if(w->wind_mph<1.f) snprintf(b,sizeof(b),"Calm");
        else snprintf(b,sizeof(b),"%.0f mph %s",w->wind_mph,isfinite(w->wind_dir_deg)?compass8(w->wind_dir_deg):"");
    } else snprintf(b,sizeof(b),"--");
    lv_label_set_text(chip_value[CHIP_WIND],b);
    if(w->precip_chance>=0) snprintf(b,sizeof(b),"%d%%",w->precip_chance); else snprintf(b,sizeof(b),"--");
    lv_label_set_text(chip_value[CHIP_RAIN],b);

    /* Sun chip: the next sun event (sunset by day, sunrise by night) */
    char upd[24]="";
    clock_12h(w->is_day?w->sunset:w->sunrise,t,sizeof(t));
    lv_label_set_text(sun_chip_name,w->is_day?"SUNSET":"SUNRISE");
    lv_label_set_text(chip_value[CHIP_SUN],t[0]?t:"--");

    for(int i=0;i<SF_WEATHER_HOURS;i++){
        bool ok=w->hourly[i].valid;
        int hour=-1;
        if(ok&&sscanf(w->hourly[i].time,"%2d",&hour)==1&&hour>=0&&hour<=23)
            snprintf(t,sizeof(t),"%d %s",hour%12?hour%12:12,hour>=12?"PM":"AM");
        else snprintf(t,sizeof(t),"--");
        lv_label_set_text(forecast_time[i],t);
        draw_icon(&icon_hour[i],ok?w->hourly[i].weather_code:-1,w->hourly[i].is_day);
        snprintf(b,sizeof(b),"%.0f" DEG,w->hourly[i].temp_f);
        lv_label_set_text(forecast_temp[i],ok?b:"--");
    }

    const char *tp=strchr(w->observation_time,'T');
    clock_12h(tp?tp+1:NULL,t,sizeof(t));
    if(t[0]) snprintf(upd,sizeof(upd),"Updated %s",t);
    lv_label_set_text(footer_label,upd);
}
void sf_weather_set_clock(const char *text){
    if(clock_label&&strcmp(lv_label_get_text(clock_label),text?text:""))lv_label_set_text(clock_label,text?text:"");
}

/* ---- Severe-weather alerts on the weather page ------------------------------
   The card takes the place of the humidity / wind / rain chips; tapping it opens the full
   National Weather Service text over the page. */
static const uint32_t alert_color[4]={0x6EB6FF,0xFFD15C,0xFF9F43,0xFF5A5A};   /* other, advisory, watch, warning */
static lv_obj_t *alert_card, *alert_icon, *alert_event, *alert_sub, *alert_page, *alert_body;
static char *alert_text;                 /* the page's text (PSRAM): too big for internal RAM */
#define ALERT_TEXT_CAP 3200
static void alert_pulse(void *obj,int32_t v){lv_obj_set_style_border_opa((lv_obj_t*)obj,(lv_opa_t)v,0);}
static bool alert_pulsing;   /* the shown alert is a warning: its card's edge breathes */
static void alert_pulse_run(void){
    if(!alert_card)return;
    bool visible=weather_root&&!lv_obj_has_flag(weather_root,LV_OBJ_FLAG_HIDDEN)&&
                 !lv_obj_has_flag(alert_card,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(alert_page,LV_OBJ_FLAG_HIDDEN);
    bool running=lv_anim_get(alert_card,alert_pulse)!=NULL;
    if(alert_pulsing&&visible&&!running){
        lv_anim_t an;lv_anim_init(&an);
        lv_anim_set_var(&an,alert_card);lv_anim_set_exec_cb(&an,alert_pulse);
        lv_anim_set_values(&an,LV_OPA_COVER,LV_OPA_20);lv_anim_set_time(&an,900);
        lv_anim_set_playback_time(&an,900);lv_anim_set_repeat_count(&an,LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&an);
    }else if((!alert_pulsing||!visible)&&running){
        lv_anim_del(alert_card,alert_pulse);
        lv_obj_set_style_border_opa(alert_card,LV_OPA_COVER,0);
    }
}
void sf_weather_close_alert(void){
    if(alert_page&&!lv_obj_has_flag(alert_page,LV_OBJ_FLAG_HIDDEN))lv_obj_add_flag(alert_page,LV_OBJ_FLAG_HIDDEN);
    alert_pulse_run();
}
bool sf_weather_alert_open(void){return alert_page&&!lv_obj_has_flag(alert_page,LV_OBJ_FLAG_HIDDEN);}
static void alert_open(lv_event_t *e){
    (void)e;
    if(!alert_page||!alert_text)return;
    lv_obj_scroll_to_y(lv_obj_get_parent(alert_body),0,LV_ANIM_OFF);
    lv_obj_clear_flag(alert_page,LV_OBJ_FLAG_HIDDEN);
    alert_pulse_run();   /* covered by the page: stop */
}
static void alert_close(lv_event_t *e){(void)e;sf_weather_close_alert();}

/* Swiping the card sideways hides it for a while (the chips come back). A different alert,
   or a more severe one, shows at once. */
#define ALERT_CARD_X 70
#define ALERT_SWIPE_PX 90                /* let go this far out: hidden */
#define SF_ALERT_HIDE_MS (5U*60U*1000U)  /* a swiped-away alert comes back after 5 minutes */
static bool alert_dismissed;             /* hidden by a swipe (alert_hide_timer brings it back) */
static char alert_dismissed_event[64];
static char alert_shown_event[64];       /* the full name (the label may end in "...") */
static uint8_t alert_dismissed_level;
static lv_timer_t *alert_hide_timer;
static lv_coord_t alert_press_x, alert_press_y;
static bool alert_dragging, alert_swallow_click;
static void alert_chips(bool show){
    for(int i=0;i<CHIP_COUNT;i++){
        if(show)lv_obj_clear_flag(lv_obj_get_parent(chip_value[i]),LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lv_obj_get_parent(chip_value[i]),LV_OBJ_FLAG_HIDDEN);
    }
}
static void alert_card_x(void *obj,int32_t x){
    lv_obj_set_x((lv_obj_t*)obj,(lv_coord_t)x);
    int32_t d=LV_ABS(x-ALERT_CARD_X);
    lv_obj_set_style_opa((lv_obj_t*)obj,(lv_opa_t)(d>=300?LV_OPA_TRANSP:LV_OPA_COVER-d*LV_OPA_COVER/300),0);
}
static void alert_unhide(lv_timer_t *t){
    (void)t;alert_hide_timer=NULL;
    if(!alert_dismissed)return;
    alert_dismissed=false;
    if(!alert_card)return;
    if(lv_label_get_text(alert_event)[0]){   /* still an alert to show */
        alert_card_x(alert_card,ALERT_CARD_X);
        lv_obj_clear_flag(alert_card,LV_OBJ_FLAG_HIDDEN);
        alert_chips(false);
        alert_pulse_run();
    }
}
static void alert_slid_out(lv_anim_t *a){
    (void)a;
    lv_obj_add_flag(alert_card,LV_OBJ_FLAG_HIDDEN);
    alert_card_x(alert_card,ALERT_CARD_X);
    alert_chips(true);
    alert_pulse_run();
}
static void alert_slide(int32_t to,bool out){
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,alert_card);lv_anim_set_exec_cb(&a,alert_card_x);
    lv_anim_set_values(&a,lv_obj_get_x(alert_card),to);
    lv_anim_set_time(&a,out?220:180);
    lv_anim_set_path_cb(&a,out?lv_anim_path_ease_in:lv_anim_path_ease_out);
    if(out)lv_anim_set_ready_cb(&a,alert_slid_out);
    lv_anim_start(&a);
}
static void alert_drag(lv_event_t *e){
    lv_event_code_t code=lv_event_get_code(e);
    lv_indev_t *in=lv_indev_get_act();
    if(!in)return;
    lv_point_t p;lv_indev_get_point(in,&p);
    if(alert_dismissed){alert_swallow_click=true;return;}   /* sliding away: leave it be */
    if(code==LV_EVENT_PRESSED){
        alert_press_x=p.x;alert_press_y=p.y;alert_dragging=false;alert_swallow_click=false;
        lv_anim_del(alert_card,alert_card_x);
    }else if(code==LV_EVENT_PRESSING){
        lv_coord_t dx=p.x-alert_press_x, dy=p.y-alert_press_y;
        if(!alert_dragging&&LV_ABS(dx)>10&&LV_ABS(dx)>LV_ABS(dy))alert_dragging=alert_swallow_click=true;
        if(alert_dragging)alert_card_x(alert_card,ALERT_CARD_X+dx);
    }else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){
        if(!alert_dragging)return;
        alert_dragging=false;
        lv_coord_t dx=lv_obj_get_x(alert_card)-ALERT_CARD_X;
        if(LV_ABS(dx)<ALERT_SWIPE_PX){alert_slide(ALERT_CARD_X,false);return;}
        /* hidden: remember which alert, come back after a while */
        alert_dismissed=true;
        snprintf(alert_dismissed_event,sizeof(alert_dismissed_event),"%s",alert_shown_event);
        if(alert_hide_timer)lv_timer_del(alert_hide_timer);
        alert_hide_timer=lv_timer_create(alert_unhide,SF_ALERT_HIDE_MS,NULL);
        lv_timer_set_repeat_count(alert_hide_timer,1);
        alert_slide(dx>0?ALERT_CARD_X+420:ALERT_CARD_X-420,true);
    }
}
static void alert_clicked(lv_event_t *e){
    if(alert_swallow_click){alert_swallow_click=false;return;}   /* the end of a swipe */
    alert_open(e);
}
static void build_weather_alerts(void){
    alert_card=lv_obj_create(weather_root);plain_obj(alert_card);
    lv_obj_set_pos(alert_card,ALERT_CARD_X,254);lv_obj_set_size(alert_card,340,62);
    lv_obj_set_style_radius(alert_card,18,0);
    lv_obj_set_style_bg_opa(alert_card,LV_OPA_20,0);
    lv_obj_set_style_border_width(alert_card,2,0);
    lv_obj_add_flag(alert_card,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(alert_card,alert_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(alert_card,alert_drag,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(alert_card,alert_drag,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(alert_card,alert_drag,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(alert_card,alert_drag,LV_EVENT_PRESS_LOST,NULL);
    alert_icon=plain_label(alert_card,LV_SYMBOL_WARNING,WHITE,&lv_font_montserrat_20,12,19,28);
    alert_event=plain_label(alert_card,"",WHITE,&lv_font_montserrat_16,48,10,280);
    lv_obj_set_style_text_align(alert_event,LV_TEXT_ALIGN_LEFT,0);
    lv_label_set_long_mode(alert_event,LV_LABEL_LONG_DOT);
    lv_obj_set_height(alert_event,lv_font_get_line_height(&lv_font_montserrat_16));
    alert_sub=plain_label(alert_card,"",MUTED,&lv_font_montserrat_12,48,34,280);
    lv_obj_set_style_text_align(alert_sub,LV_TEXT_ALIGN_LEFT,0);
    lv_label_set_long_mode(alert_sub,LV_LABEL_LONG_DOT);
    lv_obj_set_height(alert_sub,lv_font_get_line_height(&lv_font_montserrat_12));
    lv_obj_add_flag(alert_card,LV_OBJ_FLAG_HIDDEN);

    /* Full text: a dark page over the weather, scrolling between y 84 and 380. */
    alert_page=lv_obj_create(weather_root);plain_obj(alert_page);
    lv_obj_set_size(alert_page,480,480);
    lv_obj_set_style_bg_color(alert_page,lv_color_hex(0x0B1222),0);lv_obj_set_style_bg_opa(alert_page,LV_OPA_COVER,0);
    lv_obj_add_flag(alert_page,LV_OBJ_FLAG_CLICKABLE);   /* taps stay on the page */
    lv_obj_t *head=plain_label(alert_page,LV_SYMBOL_WARNING "  NATIONAL WEATHER SERVICE",lv_color_hex(0xFFD184),
                               &lv_font_montserrat_12,90,54,300);
    lv_obj_set_style_text_letter_space(head,1,0);
    lv_obj_t *box=lv_obj_create(alert_page);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box,70,80);lv_obj_set_size(box,340,300);
    lv_obj_set_scroll_dir(box,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(box,12,0);
    alert_body=lv_label_create(box);
    lv_obj_set_width(alert_body,340);
    lv_label_set_recolor(alert_body,true);
    lv_obj_set_style_text_color(alert_body,WHITE,0);
    lv_obj_set_style_text_line_space(alert_body,3,0);
    lv_label_set_text_static(alert_body,"");
    lv_obj_t *close=lv_btn_create(alert_page);
    lv_obj_set_size(close,150,46);lv_obj_set_pos(close,165,392);
    lv_obj_set_style_radius(close,23,0);lv_obj_set_style_shadow_width(close,0,0);
    lv_obj_set_style_bg_color(close,lv_color_hex(0xFFD184),0);
    lv_obj_add_event_cb(close,alert_close,LV_EVENT_CLICKED,NULL);
    lv_obj_center(text_label(close,"Close",lv_color_hex(0x241A00),&lv_font_montserrat_16));
    lv_obj_add_flag(alert_page,LV_OBJ_FLAG_HIDDEN);
}
/* Append text with LVGL's recolor marker '#' made harmless. */
static size_t alert_append(size_t n,const char *s){
    while(s&&*s&&n+1<ALERT_TEXT_CAP){alert_text[n++]=*s=='#'?' ':*s;s++;}
    alert_text[n]=0;
    return n;
}
void sf_weather_set_alerts(const sf_weather_alert_view_t *a,int count,int total){
    if(!weather_root)return;
    if(!alert_card)build_weather_alerts();
    if(!a||count<=0){
        alert_pulsing=false;
        alert_dismissed=false;
        if(alert_hide_timer){lv_timer_del(alert_hide_timer);alert_hide_timer=NULL;}
        lv_label_set_text(alert_event,"");
        lv_obj_add_flag(alert_card,LV_OBJ_FLAG_HIDDEN);
        sf_weather_close_alert();
        alert_chips(true);
        return;
    }
    const char *first=a[0].event?a[0].event:"Weather alert";
    if(alert_dismissed&&(strcmp(first,alert_dismissed_event)||(a[0].level&3)>alert_dismissed_level)){
        alert_dismissed=false;   /* a new or more severe alert: show it now */
        if(alert_hide_timer){lv_timer_del(alert_hide_timer);alert_hide_timer=NULL;}
    }
    if(!alert_text&&!(alert_text=heap_caps_malloc(ALERT_TEXT_CAP,MALLOC_CAP_SPIRAM)))return;
    lv_color_t c=lv_color_hex(alert_color[a[0].level&3]);
    if(!alert_dismissed)alert_dismissed_level=a[0].level&3;
    lv_obj_set_style_bg_color(alert_card,c,0);
    lv_obj_set_style_border_color(alert_card,c,0);
    lv_obj_set_style_border_opa(alert_card,LV_OPA_COVER,0);
    lv_obj_set_style_text_color(alert_icon,c,0);
    lv_label_set_text(alert_event,first);
    snprintf(alert_shown_event,sizeof(alert_shown_event),"%s",first);
    char sub[96];
    if(total>1)snprintf(sub,sizeof(sub),"%s%s+%d more  \xE2\x80\xA2  Tap to read",a[0].until&&a[0].until[0]?a[0].until:"",
                        a[0].until&&a[0].until[0]?"  \xE2\x80\xA2  ":"",total-1);
    else snprintf(sub,sizeof(sub),"%s%sTap to read",a[0].until&&a[0].until[0]?a[0].until:"",a[0].until&&a[0].until[0]?"  \xE2\x80\xA2  ":"");
    lv_label_set_text(alert_sub,sub);
    if(alert_dismissed){lv_obj_add_flag(alert_card,LV_OBJ_FLAG_HIDDEN);alert_chips(true);}
    else{lv_obj_clear_flag(alert_card,LV_OBJ_FLAG_HIDDEN);alert_chips(false);}
    alert_pulsing=a[0].level>=3;   /* a warning: the card's edge breathes */
    alert_pulse_run();
    /* The full page: each alert's name in its color, when it ends, where, and the text. */
    size_t n=0;
    for(int i=0;i<count;i++){
        char head[96];
        if(i)n=alert_append(n,"\n\n");
        /* the color tag is written directly: alert_append blanks every '#' */
        int k=snprintf(head,sizeof(head),"#%06lX ",(unsigned long)alert_color[a[i].level&3]);
        if(k>0&&n+(size_t)k+1<ALERT_TEXT_CAP){memcpy(alert_text+n,head,(size_t)k);n+=(size_t)k;alert_text[n]=0;}
        n=alert_append(n,a[i].event);
        /* the closing '#' ends the color */
        if(n+2<ALERT_TEXT_CAP){alert_text[n++]='#';alert_text[n++]='\n';alert_text[n]=0;}
        if(a[i].until&&a[i].until[0]){n=alert_append(n,a[i].until);n=alert_append(n,"\n");}
        if(a[i].area&&a[i].area[0]){n=alert_append(n,"For ");n=alert_append(n,a[i].area);n=alert_append(n,"\n");}
        n=alert_append(n,"\n");
        n=alert_append(n,a[i].text);
    }
    if(total>count){
        char more[64];snprintf(more,sizeof(more),"\n\n%d more alert%s in effect.",total-count,total-count>1?"s":"");
        n=alert_append(n,more);
    }
    lv_label_set_text_static(alert_body,alert_text);
}
/* Photos hold for 10 s; the double-buffered frames stay valid through any transition. */
static lv_obj_t *frame_image(lv_obj_t *frame){return frame==frame_a?picture_a:picture_b;}
static void frame_reset(lv_obj_t *frame){
    lv_anim_del(frame,NULL);
    lv_obj_set_pos(frame,0,0);lv_obj_set_size(frame,480,480);
    lv_obj_set_style_radius(frame,0,0);lv_obj_set_style_clip_corner(frame,false,0);
    lv_obj_t *img=frame_image(frame);
    lv_anim_del(img,NULL);
    lv_obj_set_pos(img,0,0);lv_obj_set_style_img_opa(img,LV_OPA_COVER,0);
}
static void transition_done(lv_anim_t *a){
    lv_obj_t *incoming=(lv_obj_t*)a->var, *old=(lv_obj_t*)a->user_data;
    if(lv_obj_get_parent(incoming)!=photos_root)incoming=lv_obj_get_parent(incoming);  /* fade animates the image */
    lv_obj_add_flag(old,LV_OBJ_FLAG_HIDDEN);
    frame_reset(old);
    frame_reset(incoming);
}
static void anim_img_opa(void *img,int32_t v){lv_obj_set_style_img_opa((lv_obj_t*)img,(lv_opa_t)v,0);}
static void anim_x(void *obj,int32_t v){lv_obj_set_x((lv_obj_t*)obj,(lv_coord_t)v);}
static void anim_y(void *obj,int32_t v){lv_obj_set_y((lv_obj_t*)obj,(lv_coord_t)v);}
static void anim_width(void *obj,int32_t v){lv_obj_set_width((lv_obj_t*)obj,(lv_coord_t)v);}
/* Doors: the new photo opens from a vertical slit in the middle out to both edges; the
   image is counter-shifted so it stays put while its window widens. */
static void anim_doors(void *obj,int32_t v){
    lv_obj_t *frame=(lv_obj_t*)obj;
    lv_coord_t off=(lv_coord_t)((480-v)/2);
    lv_obj_set_x(frame,off);lv_obj_set_width(frame,(lv_coord_t)v);
    lv_obj_set_x(frame_image(frame),(lv_coord_t)-off);
}
/* Blinds: the old photo is cut into vertical slats over the new one, and every slat
   narrows to nothing. The slats only point at the old photo's pixels; no copy is made. */
static void anim_blinds(void *obj,int32_t v){
    (void)obj;
    for(int k=0;k<BLINDS;k++)lv_obj_set_width(blind[k],(lv_coord_t)v);
}
static void blinds_stop(void){
    if(!blind[0])return;
    lv_anim_del(blind[0],NULL);
    for(int k=0;k<BLINDS;k++)lv_obj_add_flag(blind[k],LV_OBJ_FLAG_HIDDEN);
}
static void blinds_done(lv_anim_t *a){
    lv_obj_t *old=(lv_obj_t*)a->user_data, *incoming=old==frame_a?frame_b:frame_a;
    blinds_stop();
    frame_reset(old);
    frame_reset(incoming);
}
static void blinds_start(lv_obj_t *old){
    const void *src=lv_img_get_src(frame_image(old));
    for(int k=0;k<BLINDS;k++){
        lv_img_set_src(blind_img[k],src);
        lv_obj_clear_flag(blind[k],LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(blind[k]);
    }
    lv_obj_add_flag(old,LV_OBJ_FLAG_HIDDEN);   /* the slats show it now */
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,blind[0]);lv_anim_set_exec_cb(&a,anim_blinds);
    lv_anim_set_values(&a,480/BLINDS,0);lv_anim_set_time(&a,900);lv_anim_set_path_cb(&a,lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a,old);lv_anim_set_ready_cb(&a,blinds_done);
    anim_blinds(NULL,480/BLINDS);
    lv_anim_start(&a);
}
/* Iris: a centered circle grows to the full screen; the image is counter-shifted so it
   stays fixed while its round window opens. */
static void anim_iris(void *obj,int32_t v){
    lv_obj_t *frame=(lv_obj_t*)obj;
    lv_coord_t off=(lv_coord_t)((480-v)/2);
    lv_obj_set_pos(frame,off,off);lv_obj_set_size(frame,(lv_coord_t)v,(lv_coord_t)v);
    lv_obj_set_pos(frame_image(frame),(lv_coord_t)-off,(lv_coord_t)-off);
}
static void start_anim(void *var,lv_anim_exec_xcb_t cb,int32_t from,int32_t to,uint32_t ms,
                       lv_anim_path_cb_t path,lv_obj_t *old,bool notify){
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,var);lv_anim_set_exec_cb(&a,cb);
    lv_anim_set_values(&a,from,to);lv_anim_set_time(&a,ms);lv_anim_set_path_cb(&a,path);
    if(notify){lv_anim_set_user_data(&a,old);lv_anim_set_ready_cb(&a,transition_done);}
    cb(var,from);   /* apply the first step now so nothing flashes before the first tick */
    lv_anim_start(&a);
}
/* Page turn (after a swipe). The current photo is a page whose free edge is pulled across
   to the far side. Folded at the midpoint between its edge and the edge's start, the page
   shows its paper back over the part still lying flat, and the next photo shows through
   where it has lifted. v runs 0..960: how far the edge has traveled. */
static lv_obj_t *page_old;
static bool page_rightward;          /* swipe right: the page turns from the left edge */
static void anim_page(void *var,int32_t v){
    (void)var;
    int fold=480-(int)v/2;           /* width of the page still lying flat */
    int edge=480-(int)v;             /* the turned edge; past 0 the back covers all the flat part */
    int back=fold-(edge>0?edge:0);   /* width of the paper back showing */
    int x=page_rightward?480-fold:0; /* the flat part stays at the side it is fixed to */
    lv_obj_set_pos(page_old,(lv_coord_t)x,0);lv_obj_set_size(page_old,(lv_coord_t)fold,480);
    lv_obj_set_pos(frame_image(page_old),(lv_coord_t)-x,0);
    lv_obj_set_pos(page_flap,(lv_coord_t)(page_rightward?x:fold-back),0);
    lv_obj_set_size(page_flap,(lv_coord_t)back,480);
}
static void page_done(lv_anim_t *a){
    lv_obj_t *old=(lv_obj_t*)a->user_data, *incoming=old==frame_a?frame_b:frame_a;
    lv_obj_add_flag(page_flap,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(old,LV_OBJ_FLAG_HIDDEN);
    frame_reset(old);
    frame_reset(incoming);
}
static void page_turn(lv_obj_t *old,bool rightward){
    page_old=old;page_rightward=rightward;
    /* Paper back: bright at the turned edge, shaded toward the fold. */
    lv_color_t edge=lv_color_hex(0xF4F1EA), fold=lv_color_hex(0xB9B4AA);
    lv_obj_set_style_bg_color(page_flap,rightward?fold:edge,0);
    lv_obj_set_style_bg_grad_color(page_flap,rightward?edge:fold,0);
    lv_obj_move_foreground(old);          /* the page being turned lies on top of the next photo */
    lv_obj_clear_flag(page_flap,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page_flap);
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,page_flap);lv_anim_set_exec_cb(&a,anim_page);
    lv_anim_set_values(&a,0,960);lv_anim_set_time(&a,900);lv_anim_set_path_cb(&a,lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a,old);lv_anim_set_ready_cb(&a,page_done);
    anim_page(page_flap,0);
    lv_anim_start(&a);
}
static void photo_swiped(lv_event_t *e){
    (void)e;
    lv_dir_t dir=lv_indev_get_gesture_dir(lv_indev_get_act());
    if(dir!=LV_DIR_LEFT&&dir!=LV_DIR_RIGHT)return;
    swiped=true;
    if(photo_shown)swipe_turn=dir;   /* turns as soon as the next photo is decoded */
}
static void photo_present(const lv_img_dsc_t *image,bool instant,lv_dir_t turn){
    if(!frame_a||!frame_b||!image)return;
    lv_obj_t *old=picture_a_active?frame_a:frame_b;
    lv_obj_t *incoming=picture_a_active?frame_b:frame_a;
    lv_anim_del(page_flap,NULL);lv_obj_add_flag(page_flap,LV_OBJ_FLAG_HIDDEN);
    blinds_stop();
    frame_reset(old);frame_reset(incoming);   /* finish anything still running */
    lv_img_set_src(frame_image(incoming),image);
    lv_obj_clear_flag(incoming,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(incoming);
    if(instant||!photo_shown){
        lv_obj_add_flag(old,LV_OBJ_FLAG_HIDDEN);
    }else if(turn==LV_DIR_LEFT||turn==LV_DIR_RIGHT){
        page_turn(old,turn==LV_DIR_RIGHT);
    }else switch(transition_order[next_transition++%(sizeof(transition_order)/sizeof(transition_order[0]))]){
    case TR_FADE:   /* new photo fades in over the current one, which stays fully visible */
        start_anim(frame_image(incoming),anim_img_opa,LV_OPA_TRANSP,LV_OPA_COVER,1000,lv_anim_path_ease_in_out,old,true);
        break;
    case TR_SLIDE:  /* push: both move left together */
        start_anim(old,anim_x,0,-480,800,lv_anim_path_ease_in_out,old,false);
        start_anim(incoming,anim_x,480,0,800,lv_anim_path_ease_in_out,old,true);
        break;
    case TR_IRIS:
        lv_obj_set_style_radius(incoming,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_clip_corner(incoming,true,0);
        start_anim(incoming,anim_iris,0,480,900,lv_anim_path_ease_out,old,true);
        break;
    case TR_PUSH_UP:     /* both move up together */
        start_anim(old,anim_y,0,-480,800,lv_anim_path_ease_in_out,old,false);
        start_anim(incoming,anim_y,480,0,800,lv_anim_path_ease_in_out,old,true);
        break;
    case TR_PUSH_RIGHT:  /* both move right together */
        start_anim(old,anim_x,0,480,800,lv_anim_path_ease_in_out,old,false);
        start_anim(incoming,anim_x,-480,0,800,lv_anim_path_ease_in_out,old,true);
        break;
    case TR_COVER_DOWN:  /* the new photo slides down over the current one */
        start_anim(incoming,anim_y,-480,0,800,lv_anim_path_ease_out,old,true);
        break;
    case TR_DOORS:
        start_anim(incoming,anim_doors,0,480,900,lv_anim_path_ease_in_out,old,true);
        break;
    case TR_BLINDS:
        blinds_start(old);
        break;
    case TR_WIPE:   /* left-to-right reveal */
    default:
        start_anim(incoming,anim_width,0,480,800,lv_anim_path_ease_in_out,old,true);
        break;
    }
    if(no_photos_label)lv_obj_move_foreground(no_photos_label);
    picture_a_active=!picture_a_active;
    photo_shown=true;
    if(no_photos_label)lv_obj_add_flag(no_photos_label,LV_OBJ_FLAG_HIDDEN);
}
void sf_photo_set_provider(sf_next_photo_cb_t cb,void *ctx){
    next_photo=cb;next_photo_ctx=ctx;have_photo_time=false;
    if(cb&&!photo_shown&&no_photos_label)lv_label_set_text(no_photos_label,"Loading photos...");
}

static lv_obj_t *usb_root;
static void (*on_usb_done)(void);
static void usb_done_clicked(lv_event_t *e){(void)e;if(on_usb_done)on_usb_done();}
static void build_usb_notice(lv_obj_t *parent){
    usb_root=screen(parent,lv_color_hex(0x0B1A2E));
    /* Simple drive glyph: a rounded box with an activity light. */
    lv_obj_t *drive=lv_obj_create(usb_root);plain_obj(drive);
    lv_obj_set_pos(drive,204,56);lv_obj_set_size(drive,72,94);
    lv_obj_set_style_radius(drive,12,0);
    lv_obj_set_style_border_color(drive,lv_color_hex(0x6EB6FF),0);
    lv_obj_set_style_border_width(drive,4,0);
    lv_obj_t *led=lv_obj_create(drive);plain_obj(led);
    lv_obj_set_pos(led,26,64);lv_obj_set_size(led,12,12);
    lv_obj_set_style_radius(led,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(led,lv_color_hex(0x7CF5A3),0);lv_obj_set_style_bg_opa(led,LV_OPA_COVER,0);
    plain_label(usb_root,"USB file transfer",WHITE,&lv_font_montserrat_24,60,166,360);
    plain_label(usb_root,"The microSD card is open as a drive\non your computer.",MUTED,LV_FONT_DEFAULT,60,206,360);
    plain_label(usb_root,"Slideshow photos go in /photos\nPlane photos go in /planes/types",
                WHITE,LV_FONT_DEFAULT,60,266,360);
    plain_label(usb_root,"Eject the drive on the computer,\nthen tap Done.",
                lv_color_hex(0xFFD184),LV_FONT_DEFAULT,60,336,360);
    /* Done: SkyFrame takes the card back even if the computer's eject didn't reach it. */
    lv_obj_t *done=lv_btn_create(usb_root);
    lv_obj_set_size(done,160,44);lv_obj_set_pos(done,160,388);
    lv_obj_set_style_radius(done,22,0);lv_obj_set_style_shadow_width(done,0,0);
    lv_obj_set_style_bg_color(done,lv_color_hex(0x7CF5A3),0);
    lv_obj_add_event_cb(done,usb_done_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_center(text_label(done,LV_SYMBOL_OK "  Done",lv_color_hex(0x06150D),&lv_font_montserrat_16));
}
void sf_screens_set_usb_done_cb(void (*cb)(void)){on_usb_done=cb;}
void sf_photo_tick(uint32_t now_ms){
    if(!photos_root||lv_obj_has_flag(photos_root,LV_OBJ_FLAG_HIDDEN)||!next_photo)return;
    /* Advance every 10 s; if the next frame is still decoding, keep the current one
       and check again on later ticks (the provider never blocks). */
    if(swipe_turn!=LV_DIR_NONE||!have_photo_time|| (uint32_t)(now_ms-last_photo_ms)>=10000u){
        sf_photo_status_t state=SF_PHOTOS_LOADING;
        const lv_img_dsc_t *frame=next_photo(next_photo_ctx,&state);
        /* Entering the slideshow: cut straight to a fresh photo rather than showing the
           last cycle's photo and immediately transitioning away from it. A swipe turns
           the page; otherwise the slideshow's own transitions take turns. */
        if(frame){
            photo_present(frame,show_instant,swipe_turn);
            show_instant=false;swipe_turn=LV_DIR_NONE;last_photo_ms=now_ms;have_photo_time=true;
        }
        else if(!photo_shown&&no_photos_label){
            static const char *const text[]={
                [SF_PHOTOS_LOADING]="Loading photos...",
                [SF_PHOTOS_OK]="Loading photos...",
                [SF_PHOTOS_NO_CARD]="No microSD card\n\nInsert a FAT32 card with a\n/photos folder and restart",
                [SF_PHOTOS_NONE]="No photos found\n\nAdd .jpg files to the /photos\nfolder on the microSD card",
                [SF_PHOTOS_UNREADABLE]="Photos could not be read\n\nUse baseline .jpg files\nunder 4 MB",
                [SF_PHOTOS_USB]="The microSD card is open\non your computer",
            };
            if((unsigned)state<sizeof(text)/sizeof(text[0])&&strcmp(lv_label_get_text(no_photos_label),text[state]))
                lv_label_set_text(no_photos_label,text[state]);
        }
    }
}

void sf_usb_show(bool show){
    if(!usb_root||show==!lv_obj_has_flag(usb_root,LV_OBJ_FLAG_HIDDEN))return;
    if(show)lv_obj_clear_flag(usb_root,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(usb_root,LV_OBJ_FLAG_HIDDEN);
}
void sf_screens_set_long_press_cb(void (*cb)(void)){on_long_press=cb;}
void sf_screens_set_hold_cb(void (*cb)(void)){on_hold=cb;}
void sf_screens_set_setup_close_cb(void (*cb)(void)){on_setup_close=cb;}

/* ---- Verse of the day ---------------------------------------------------- */
static lv_obj_t *verse_root, *verse_box, *verse_text, *verse_ref, *verse_credit;
static void build_verse(lv_obj_t *parent){
    verse_root=screen(parent,lv_color_hex(0x16263F));
    lv_obj_set_style_bg_grad_color(verse_root,lv_color_hex(0x080E1C),0);
    lv_obj_set_style_bg_grad_dir(verse_root,LV_GRAD_DIR_VER,0);
    sf_gesture_attach(verse_root);
    plain_label(verse_root,"VERSE OF THE DAY",lv_color_hex(0xD9B76A),&lv_font_montserrat_14,120,61,240);
    lv_obj_t *rule=lv_obj_create(verse_root);plain_obj(rule);
    lv_obj_set_pos(rule,210,84);lv_obj_set_size(rule,60,2);
    lv_obj_set_style_bg_color(rule,lv_color_hex(0xD9B76A),0);lv_obj_set_style_bg_opa(rule,LV_OPA_COVER,0);
    verse_box=lv_obj_create(verse_root);plain_obj(verse_box);
    lv_obj_set_width(verse_box,340);lv_obj_set_height(verse_box,LV_SIZE_CONTENT);
    lv_obj_set_pos(verse_box,70,120);
    lv_obj_set_flex_flow(verse_box,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(verse_box,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(verse_box,16,0);
    verse_text=lv_label_create(verse_box);
    lv_obj_set_width(verse_text,340);
    lv_obj_set_style_text_align(verse_text,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(verse_text,WHITE,0);
    lv_obj_set_style_text_line_space(verse_text,4,0);
    verse_ref=lv_label_create(verse_box);
    lv_obj_set_style_text_color(verse_ref,lv_color_hex(0xD9B76A),0);
    lv_obj_set_style_text_font(verse_ref,&lv_font_montserrat_16,0);
    /* Required credit, kept but as small as text can be drawn here (8 px). */
    verse_credit=plain_label(verse_root,"",FAINT,&lv_font_montserrat_8,140,434,200);
}
void sf_verse_set(const sf_verse_t *v){
    if(!verse_root||!v||!v->valid)return;
    size_t n=strlen(v->text);
    /* Largest font that keeps the passage inside the round glass: start from a guess by
       length, then step down below until it measures short enough. */
    const lv_font_t *fonts[]={&lv_font_montserrat_24,&lv_font_montserrat_20,&lv_font_montserrat_16,LV_FONT_DEFAULT,&lv_font_montserrat_12};
    int f=n<=110?0:n<=200?1:n<=340?2:3;
    lv_label_set_text_fmt(verse_text,"\"%s\"",v->text);
    lv_label_set_text_fmt(verse_ref,"%s  \xE2\x80\xA2  %s",v->reference,v->version);   /* "Psalm 95:1-2 * NKJV" */
    lv_label_set_text(verse_credit,v->credit);
    /* Smaller font until the passage and reference fit between y=96 and the credit. */
    int h;
    for(;;){
        lv_obj_set_style_text_font(verse_text,fonts[f],0);
        lv_obj_update_layout(verse_root);
        h=lv_obj_get_height(verse_box);
        if(h<=328||f==4)break;
        f++;
    }
    int y=240-h/2;
    if(y>424-h)y=424-h;
    if(y<96)y=96;
    lv_obj_set_y(verse_box,y);
}
void sf_verse_show(bool show){
    if(!verse_root)return;
    if(show)lv_obj_clear_flag(verse_root,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(verse_root,LV_OBJ_FLAG_HIDDEN);
}

/* ---- Birthday reminder --------------------------------------------------- */
static lv_obj_t *bday_root, *bday_frame, *bday_img, *bday_initial, *bday_title, *bday_name;
static lv_obj_t *bday_age, *bday_note, *bday_btn, *bday_btn_text;
static lv_img_dsc_t bday_dsc;
#define BDAY_GOLD lv_color_hex(0xFFC857)
static void bday_dismiss_clicked(lv_event_t *e){(void)e;if(on_birthday_dismiss)on_birthday_dismiss();}
static void build_birthday(lv_obj_t *parent){
    bday_root=screen(parent,lv_color_hex(0x3B1F4E));
    lv_obj_set_style_bg_grad_color(bday_root,lv_color_hex(0x120A22),0);
    lv_obj_set_style_bg_grad_dir(bday_root,LV_GRAD_DIR_VER,0);
    sf_gesture_attach(bday_root);
    /* Confetti around the rim. */
    static const uint32_t colors[]={0xFFC857,0xFF7AA8,0x6EDCFF,0x7CF5A3,0xFFFFFF,0xC89BFF};
    for(int i=0;i<30;i++){
        float a=(float)i*12.f*3.14159265f/180.f+(i%3)*0.07f;
        float r=203.f+(float)((i*37)%22);
        int s=5+(i*7)%6;
        lv_obj_t *c=lv_obj_create(bday_root);plain_obj(c);
        lv_obj_set_size(c,s,i%2?s:s/2+2);
        lv_obj_set_pos(c,240+(int)lroundf(sinf(a)*r)-s/2,240-(int)lroundf(cosf(a)*r)-s/2);
        lv_obj_set_style_radius(c,i%2?LV_RADIUS_CIRCLE:1,0);
        lv_obj_set_style_bg_color(c,lv_color_hex(colors[i%6]),0);lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
    }
    /* Round photo frame (or the person's initial when there is no photo). */
    bday_frame=lv_obj_create(bday_root);plain_obj(bday_frame);
    lv_obj_set_size(bday_frame,180,180);
    lv_obj_set_style_radius(bday_frame,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_clip_corner(bday_frame,true,0);
    lv_obj_set_style_bg_color(bday_frame,lv_color_hex(0x5A3473),0);lv_obj_set_style_bg_opa(bday_frame,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(bday_frame,BDAY_GOLD,0);lv_obj_set_style_border_width(bday_frame,4,0);
    lv_obj_set_style_border_post(bday_frame,true,0);   /* the ring over the photo, all the way round */
    bday_initial=lv_label_create(bday_frame);
    lv_obj_set_style_text_font(bday_initial,&lv_font_montserrat_48,0);
    lv_obj_set_style_text_color(bday_initial,WHITE,0);
    lv_obj_center(bday_initial);
    bday_img=lv_img_create(bday_frame);
    lv_obj_set_pos(bday_img,-4,-4);   /* children start inside the border: back to the frame's corner */
    bday_title=plain_label(bday_root,"",WHITE,&lv_font_montserrat_24,40,0,400);
    lv_label_set_long_mode(bday_title,LV_LABEL_LONG_DOT);
    lv_obj_set_height(bday_title,lv_font_get_line_height(&lv_font_montserrat_24));
    bday_name=plain_label(bday_root,"",WHITE,&lv_font_montserrat_48,40,0,400);
    lv_label_set_long_mode(bday_name,LV_LABEL_LONG_DOT);
    lv_obj_set_height(bday_name,lv_font_get_line_height(&lv_font_montserrat_48));
    bday_age=plain_label(bday_root,"",BDAY_GOLD,&lv_font_montserrat_20,60,0,360);
    bday_note=plain_label(bday_root,"",MUTED,&lv_font_montserrat_16,60,0,360);
    bday_btn=lv_btn_create(bday_root);
    lv_obj_set_height(bday_btn,46);
    lv_obj_set_style_radius(bday_btn,23,0);
    lv_obj_set_style_bg_color(bday_btn,BDAY_GOLD,0);
    lv_obj_set_style_shadow_width(bday_btn,0,0);
    lv_obj_add_event_cb(bday_btn,bday_dismiss_clicked,LV_EVENT_CLICKED,NULL);
    bday_btn_text=lv_label_create(bday_btn);
    lv_obj_set_style_text_color(bday_btn_text,lv_color_hex(0x2A1238),0);
    lv_obj_set_style_text_font(bday_btn_text,&lv_font_montserrat_16,0);
    lv_obj_center(bday_btn_text);
}
void sf_birthday_set(const sf_birthday_view_t *v){
    if(!bday_root||!v)return;
    static const char *const months[]={"January","February","March","April","May","June","July",
                                       "August","September","October","November","December"};
    /* Photo, or the first letter of the name. */
    if(v->pixels&&v->photo_size==180){
        memset(&bday_dsc,0,sizeof(bday_dsc));
        bday_dsc.header.cf=LV_IMG_CF_TRUE_COLOR;bday_dsc.header.w=180;bday_dsc.header.h=180;
        bday_dsc.data_size=180u*180u*2u;bday_dsc.data=(const uint8_t*)v->pixels;
        lv_img_cache_invalidate_src(&bday_dsc);
        lv_img_set_src(bday_img,&bday_dsc);
        lv_obj_clear_flag(bday_img,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bday_initial,LV_OBJ_FLAG_HIDDEN);
    }else{
        lv_obj_add_flag(bday_img,LV_OBJ_FLAG_HIDDEN);
        char initial[2]={v->name&&v->name[0]?(char)toupper((unsigned char)v->name[0]):'?',0};
        lv_label_set_text(bday_initial,initial);
        lv_obj_clear_flag(bday_initial,LV_OBJ_FLAG_HIDDEN);
    }
    const char *month=(v->month>=1&&v->month<=12)?months[v->month-1]:"";
    if(v->owner){
        /* The owner's own birthday: larger, warmer, a personal message. */
        lv_obj_set_pos(bday_frame,150,38);
        if(v->age>0) lv_label_set_text_fmt(bday_title,"Happy %d%s Birthday,",v->age,sf_ordinal_suffix(v->age));
        else lv_label_set_text(bday_title,"Happy Birthday,");
        lv_obj_set_style_text_color(bday_title,BDAY_GOLD,0);
        lv_obj_set_y(bday_title,228);
        lv_label_set_text_fmt(bday_name,"%s!",v->name);
        lv_obj_set_y(bday_name,258);
        lv_obj_clear_flag(bday_name,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bday_age,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(bday_note,v->message?v->message:"");
        lv_obj_set_y(bday_note,322);
        lv_label_set_text(bday_btn_text,"Thank you!");
        lv_obj_set_width(bday_btn,180);
        lv_obj_set_pos(bday_btn,150,364);
    }else{
        lv_obj_set_pos(bday_frame,150,52);
        lv_label_set_text_fmt(bday_title,"Happy Birthday, %s!",v->name);
        lv_obj_set_style_text_color(bday_title,WHITE,0);
        lv_obj_set_y(bday_title,246);
        lv_obj_add_flag(bday_name,LV_OBJ_FLAG_HIDDEN);
        if(v->age>0) lv_label_set_text_fmt(bday_age,"Turning %d today",v->age);
        else lv_label_set_text(bday_age,"Celebrating today");
        lv_obj_set_y(bday_age,280);
        lv_obj_clear_flag(bday_age,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(bday_note,"%s %d",month,v->day);
        lv_obj_set_y(bday_note,312);
        lv_label_set_text(bday_btn_text,"Dismiss until next year");
        lv_obj_set_width(bday_btn,230);
        lv_obj_set_pos(bday_btn,125,356);
    }
}
void sf_birthday_show(bool show){
    if(!bday_root)return;
    if(show)lv_obj_clear_flag(bday_root,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bday_root,LV_OBJ_FLAG_HIDDEN);
}
void sf_screens_set_birthday_dismiss_cb(void (*cb)(void)){on_birthday_dismiss=cb;}

/* ---- Settings and test screens --------------------------------------------
   Both are rarely open, so they are built when shown and deleted when hidden: LVGL
   objects live in scarce internal RAM. Deletion is deferred because a tap on one of their
   own buttons can be what closes them. */
#define ACCENT_BLUE  lv_color_hex(0x6EB6FF)
#define ACCENT_GREEN lv_color_hex(0x7CF5A3)
#define ACCENT_GOLD  lv_color_hex(0xFFC857)
#define ACCENT_AMBER lv_color_hex(0xFFD184)
static lv_obj_t *overlay_screen(lv_color_t top, lv_color_t bot){
    lv_obj_t *o=screen(screens_parent,top);
    lv_obj_set_style_bg_grad_color(o,bot,0);lv_obj_set_style_bg_grad_dir(o,LV_GRAD_DIR_VER,0);
    if(usb_root)lv_obj_move_to_index(o,(int32_t)lv_obj_get_index(usb_root));   /* USB notice stays on top */
    return o;
}
static void set_text(lv_obj_t *label,const char *s){
    if(label&&strcmp(lv_label_get_text(label),s))lv_label_set_text(label,s);
}
static lv_obj_t *text_label(lv_obj_t *parent,const char *s,lv_color_t c,const lv_font_t *font){
    lv_obj_t *l=lv_label_create(parent);
    lv_label_set_text(l,s);
    lv_obj_set_style_text_color(l,c,0);
    lv_obj_set_style_text_font(l,font,0);
    return l;
}

/* ---- Setup / settings screen ---------------------------------------------
   Two steps in test mode's style: 1) a QR code that joins SkyFrame's own Wi-Fi, 2) a QR code
   that opens the settings page. Next/Back buttons or a swipe move between them. When SkyFrame
   is on the home Wi-Fi, a phone there can open the page directly: that comes first, and joining
   SkyFrame's Wi-Fi is only the fallback ("Other Wi-Fi") for a phone on another network. */
static lv_obj_t *card(lv_obj_t *parent);                      /* shared with test mode, below */
static lv_obj_t *stat_row(lv_obj_t *parent,const char *name,lv_color_t value_color);
static lv_obj_t *setup_root, *setup_step[2], *setup_step_label;
static bool setup_closable, setup_hold_fired, setup_home;
static int setup_page;                   /* kept across rebuilds while the screen stays up */
static uint32_t setup_pressed_at;
static void (*on_test_enter)(void);
static void (*on_test_action)(sf_test_action_t);
#define HOLD_FOR_TEST_MS 5000
enum { NAV_CLOSE, NAV_NEXT, NAV_BACK };
static int setup_first(void){return setup_home?1:0;}   /* the page a fresh opening shows */
static void setup_go(int page){
    setup_page=page;
    for(int i=0;i<2;i++){
        if(i==page)lv_obj_clear_flag(setup_step[i],LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(setup_step[i],LV_OBJ_FLAG_HIDDEN);
    }
    if(setup_home)lv_label_set_text(setup_step_label,page?"PHONE ON THE SAME WI-FI":"PHONE ON ANOTHER WI-FI?");
    else lv_label_set_text(setup_step_label,page?"STEP 2 OF 2  \xE2\x80\xA2  OPEN SETTINGS":"STEP 1 OF 2  \xE2\x80\xA2  JOIN SKYFRAME WI-FI");
}
static void setup_nav(lv_event_t *e){
    int a=(int)(uintptr_t)lv_event_get_user_data(e);
    if(a==NAV_CLOSE){if(setup_closable&&on_setup_close)on_setup_close();}
    else setup_go(a==NAV_NEXT?1-setup_first():setup_first());
}
/* Holding the settings screen 5 s opens test mode; a swipe changes step. */
static void setup_touch(lv_event_t *e){
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_GESTURE){
        lv_dir_t d=lv_indev_get_gesture_dir(lv_indev_get_act());
        if(d==LV_DIR_LEFT&&setup_page==setup_first())setup_go(1-setup_first());
        else if(d==LV_DIR_RIGHT&&setup_page!=setup_first())setup_go(setup_first());
        return;
    }
    if(code==LV_EVENT_PRESSED){setup_pressed_at=lv_tick_get();setup_hold_fired=false;return;}
    if(setup_closable&&!setup_hold_fired&&lv_tick_elaps(setup_pressed_at)>=HOLD_FOR_TEST_MS){
        setup_hold_fired=true;
        /* The finger is still down: it must not land on a test menu button on release. */
        lv_indev_wait_release(lv_event_get_indev(e));
        if(on_test_enter)on_test_enter();
    }
}
/* Pill button like test mode's Exit: green for the way forward, glass for the way back. */
static void setup_button(lv_obj_t *parent,const char *text,bool primary,int x,int w,int action){
    lv_obj_t *b=lv_btn_create(parent);
    lv_obj_set_size(b,w,48);lv_obj_set_pos(b,x,0);
    lv_obj_set_style_radius(b,24,0);
    lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_bg_color(b,primary?ACCENT_GREEN:WHITE,0);
    lv_obj_set_style_bg_opa(b,primary?LV_OPA_COVER:LV_OPA_20,0);
    lv_obj_add_event_cb(b,setup_nav,LV_EVENT_CLICKED,(void*)(uintptr_t)action);
    lv_obj_center(text_label(b,text,primary?lv_color_hex(0x06150D):WHITE,&lv_font_montserrat_16));
}
/* One step: QR card, a one-line instruction, a glass card of details, and the buttons. */
static lv_obj_t *setup_page_box(const char *qr_text,const char *caption){
    lv_obj_t *p=lv_obj_create(setup_root);plain_obj(p);
    lv_obj_set_size(p,480,480-92);lv_obj_set_pos(p,0,92);
    /* QR codes need a light quiet zone to scan: white card, code inset 14 px. */
    lv_obj_t *qc=lv_obj_create(p);plain_obj(qc);
    lv_obj_set_size(qc,172,172);lv_obj_set_pos(qc,154,4);
    lv_obj_set_style_radius(qc,18,0);
    lv_obj_set_style_bg_color(qc,lv_color_white(),0);lv_obj_set_style_bg_opa(qc,LV_OPA_COVER,0);
    lv_obj_t *qr=lv_qrcode_create(qc,144,lv_color_black(),lv_color_white());
    lv_obj_set_pos(qr,14,14);
    lv_obj_clear_flag(qr,LV_OBJ_FLAG_CLICKABLE);
    lv_qrcode_update(qr,qr_text,(uint32_t)strlen(qr_text));
    plain_label(p,caption,WHITE,&lv_font_montserrat_16,60,184,360);
    return p;
}
static lv_obj_t *setup_details(lv_obj_t *page){
    lv_obj_t *c=card(page);
    lv_obj_set_width(c,320);lv_obj_set_pos(c,80,210);
    return c;
}
/* Buttons along the bottom: two side by side, or one centered. */
static void setup_buttons(lv_obj_t *page,const char *left,int left_action,const char *right,int right_action){
    lv_obj_t *row=lv_obj_create(page);plain_obj(row);
    lv_obj_set_size(row,480,48);lv_obj_set_pos(row,0,294);
    if(left&&right){setup_button(row,left,false,110,124,left_action);setup_button(row,right,true,246,124,right_action);}
    else if(right)setup_button(row,right,true,160,160,right_action);
    else if(left)setup_button(row,left,false,160,160,left_action);
}
/* Layout (each row inside the circle at its height): y 32 title, 60 step, 76 subtitle,
   96-268 QR card, 276 instruction, 302-370 details card, 386-434 buttons. */
static void build_setup(const sf_setup_info_t *info){
    setup_root=overlay_screen(lv_color_hex(0x16263F),lv_color_hex(0x080E1C));   /* test mode's night blue */
    lv_obj_add_flag(setup_root,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(setup_root,LV_OBJ_FLAG_GESTURE_BUBBLE);
    static const lv_event_code_t codes[]={LV_EVENT_PRESSED,LV_EVENT_PRESSING,LV_EVENT_GESTURE};
    for(size_t i=0;i<sizeof(codes)/sizeof(codes[0]);i++)lv_obj_add_event_cb(setup_root,setup_touch,codes[i],NULL);
    bool problem=info->subtitle&&info->subtitle[0];
    char b[160];
    snprintf(b,sizeof(b),"%s  %s",problem?LV_SYMBOL_WARNING:LV_SYMBOL_SETTINGS,info->title?info->title:"SkyFrame Setup");
    plain_label(setup_root,b,WHITE,&lv_font_montserrat_20,60,32,360);   /* fits the glass at this height */
    setup_step_label=plain_label(setup_root,"",ACCENT_GOLD,&lv_font_montserrat_12,80,60,320);
    lv_obj_set_style_text_letter_space(setup_step_label,1,0);
    if(problem){   /* e.g. the network it can't join: one line however long */
        lv_obj_t *sub=plain_label(setup_root,info->subtitle,ACCENT_AMBER,&lv_font_montserrat_12,110,76,260);
        lv_label_set_long_mode(sub,LV_LABEL_LONG_DOT);
        lv_obj_set_height(sub,lv_font_get_line_height(&lv_font_montserrat_12));
    }
    bool home=setup_home;

    /* Step 1: phone cameras (iPhone and Android) join a network from this code. */
    snprintf(b,sizeof(b),"WIFI:T:WPA;S:%s;P:%s;;",info->ap_ssid,info->ap_password);
    setup_step[0]=setup_page_box(b,home?"Join this, then open 192.168.4.1":"Scan to join SkyFrame's Wi-Fi");
    lv_obj_t *c=setup_details(setup_step[0]);
    lv_label_set_text(stat_row(c,"Wi-Fi",WHITE),info->ap_ssid);
    lv_label_set_text(stat_row(c,"Password",ACCENT_GOLD),info->ap_password);
    if(home)setup_buttons(setup_step[0],LV_SYMBOL_LEFT "  Back",NAV_BACK,setup_closable?"Done  " LV_SYMBOL_OK:NULL,NAV_CLOSE);
    else setup_buttons(setup_step[0],setup_closable?LV_SYMBOL_CLOSE "  Close":NULL,NAV_CLOSE,"Next  " LV_SYMBOL_RIGHT,NAV_NEXT);

    /* Step 2: the settings page. On the home Wi-Fi its name works for any phone there; on
       SkyFrame's own network the address is the reliable one (many phones don't resolve
       ".local" names on it). */
    const char *url=home?"http://skyframe.local":"http://192.168.4.1";
    setup_step[1]=setup_page_box(url,"Scan to open the settings page");
    c=setup_details(setup_step[1]);
    lv_label_set_text(stat_row(c,"Address",ACCENT_GREEN),url+7);
    if(!home)lv_label_set_text(stat_row(c,"Phone on",WHITE),"SkyFrame's Wi-Fi");
    if(home)setup_buttons(setup_step[1],"Other Wi-Fi",NAV_NEXT,setup_closable?"Done  " LV_SYMBOL_OK:NULL,NAV_CLOSE);
    else setup_buttons(setup_step[1],LV_SYMBOL_LEFT "  Back",NAV_BACK,setup_closable?"Done  " LV_SYMBOL_OK:NULL,NAV_CLOSE);

    setup_go(setup_page);
    lv_obj_clear_flag(setup_root,LV_OBJ_FLAG_HIDDEN);
}
void sf_setup_show(bool show,const sf_setup_info_t *info){
    /* Called only when something changed (main caches it): rebuild with the new details,
       staying on the same step; a fresh opening starts at the first one. */
    bool was_up=setup_root!=NULL;
    if(setup_root){lv_obj_del_async(setup_root);setup_root=NULL;}
    setup_closable=false;
    if(!show||!info||!screens_parent){setup_page=0;return;}
    setup_closable=info->closable;
    setup_home=info->home_ip&&info->home_ip[0];
    if(!was_up)setup_page=setup_first();
    sf_lvmem_prefer_psram(true);   /* big and on demand: keep internal RAM for Wi-Fi */
    build_setup(info);
    sf_lvmem_prefer_psram(false);
}

/* ---- Test mode menu -------------------------------------------------------
   Fixed header and Exit button; between them a scrolling list of sections:
   Previews and Screens (tiles), System (stats), Web pages, Device (restart). */
#define LIST_W 340
enum { TILE_CPU, TILE_RAM, TILE_TEMP, TILE_COUNT };
enum { ROW_RAM_LOW, ROW_BLOCK, ROW_PSRAM, ROW_SD, ROW_BATTERY, ROW_WIFI, ROW_IP, ROW_UPTIME,
       ROW_BOOTS, ROW_RESET, ROW_CRASH, ROW_CLOCK, ROW_VERSION, ROW_UPDATE, ROW_ISS, ROW_COUNT };

static lv_obj_t *test_root, *test_list, *test_badge, *web_note, *test_version;
/* "Check for updates": once tapped, its label shows the check as it goes (the System
   panel's Updates row is far up the list). */
static lv_obj_t *update_button_label;
static bool update_tapped;
static lv_obj_t *tile_value[TILE_COUNT], *row_value[ROW_COUNT], *web_row;
static sf_system_stats_t test_stats;
static bool have_test_stats;
static void apply_stats(void);
static void test_clicked(lv_event_t *e){
    sf_test_action_t a=(sf_test_action_t)(uintptr_t)lv_event_get_user_data(e);
    if(a==SF_TEST_UPDATE&&update_button_label){update_tapped=true;set_text(update_button_label,"Checking for updates...");}
    if(on_test_action)on_test_action(a);
}
/* Flex container spanning the list width. */
static lv_obj_t *list_box(lv_obj_t *parent,lv_flex_flow_t flow,int gap){
    lv_obj_t *o=lv_obj_create(parent);plain_obj(o);
    lv_obj_set_size(o,LIST_W,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o,flow);
    lv_obj_set_style_pad_row(o,gap,0);lv_obj_set_style_pad_column(o,gap,0);
    return o;
}
static void section(lv_obj_t *parent,const char *title,lv_color_t c){
    lv_obj_t *l=text_label(parent,title,c,&lv_font_montserrat_12);
    lv_obj_set_style_text_letter_space(l,2,0);
    lv_obj_set_style_pad_top(l,8,0);
}
/* Glass card like the weather chips. */
static lv_obj_t *card(lv_obj_t *parent){
    lv_obj_t *c=list_box(parent,LV_FLEX_FLOW_COLUMN,4);
    lv_obj_set_style_radius(c,16,0);
    lv_obj_set_style_bg_color(c,WHITE,0);lv_obj_set_style_bg_opa(c,LV_OPA_10,0);
    lv_obj_set_style_pad_hor(c,16,0);lv_obj_set_style_pad_ver(c,12,0);
    return c;
}
/* Tile: icon above the name; a wide tile puts them side by side. */
static lv_obj_t *test_button(lv_obj_t *parent,sf_test_action_t a,const char *icon,const char *text,
                        lv_color_t icon_color,lv_color_t bg,bool wide){
    lv_obj_t *b=lv_btn_create(parent);
    lv_obj_set_size(b,wide?LIST_W:(LIST_W-10)/2,wide?52:72);
    lv_obj_set_style_radius(b,16,0);
    lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_bg_color(b,bg,0);
    lv_obj_set_style_bg_opa(b,LV_OPA_20,0);
    lv_obj_set_style_bg_opa(b,LV_OPA_40,LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b,6,0);
    lv_obj_set_flex_flow(b,wide?LV_FLEX_FLOW_ROW:LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b,4,0);lv_obj_set_style_pad_column(b,12,0);
    lv_obj_add_event_cb(b,test_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)a);
    text_label(b,icon,icon_color,&lv_font_montserrat_20);
    lv_obj_t *l=text_label(b,text,WHITE,LV_FONT_DEFAULT);
    if(wide){lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_set_style_max_width(l,LIST_W-60,0);}
    return l;
}
/* "Name ........ value" row; returns the value label. */
static lv_obj_t *stat_row(lv_obj_t *parent,const char *name,lv_color_t value_color){
    lv_obj_t *row=lv_obj_create(parent);plain_obj(row);
    lv_obj_set_size(row,LV_PCT(100),20);
    lv_obj_t *k=text_label(row,name,MUTED,LV_FONT_DEFAULT);
    lv_obj_align(k,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_t *v=text_label(row,"--",value_color,LV_FONT_DEFAULT);
    lv_label_set_long_mode(v,LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v,200,0);
    lv_obj_align(v,LV_ALIGN_RIGHT_MID,0,0);
    return v;
}
static void build_test_menu(void){
    test_root=overlay_screen(lv_color_hex(0x16263F),lv_color_hex(0x080E1C));   /* the verse page's night blue */
    /* y 30-92: header, with the exact firmware running (for support questions) */
    plain_label(test_root,LV_SYMBOL_SETTINGS "  Test mode",WHITE,&lv_font_montserrat_24,90,30,300);
    test_version=plain_label(test_root,"",ACCENT_GOLD,&lv_font_montserrat_12,80,60,320);
    lv_label_set_long_mode(test_version,LV_LABEL_LONG_DOT);
    lv_obj_set_height(test_version,lv_font_get_line_height(&lv_font_montserrat_12));
    plain_label(test_root,"Previews only  \xE2\x80\xA2  nothing is saved",FAINT,&lv_font_montserrat_12,90,76,300);

    /* y 96-390: the scrolling list, 340 wide so its corners stay inside the glass. */
    test_list=lv_obj_create(test_root);
    lv_obj_remove_style_all(test_list);
    lv_obj_set_pos(test_list,(480-LIST_W)/2,96);lv_obj_set_size(test_list,LIST_W,294);
    lv_obj_set_scroll_dir(test_list,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(test_list,LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(test_list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(test_list,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(test_list,10,0);
    lv_obj_set_style_pad_bottom(test_list,16,0);

    section(test_list,"PREVIEWS",ACCENT_GOLD);
    lv_obj_t *g=list_box(test_list,LV_FLEX_FLOW_ROW_WRAP,10);
    test_button(g,SF_TEST_OWNER_BIRTHDAY,LV_SYMBOL_BELL,"Owner's birthday",ACCENT_GOLD,WHITE,false);
    test_button(g,SF_TEST_FAMILY_BIRTHDAY,LV_SYMBOL_IMAGE,"Family birthday",ACCENT_GOLD,WHITE,false);
    test_button(g,SF_TEST_TRAFFIC,LV_SYMBOL_GPS,"Sample traffic",ACCENT_GOLD,WHITE,false);
    test_button(g,SF_TEST_NIGHT,LV_SYMBOL_EYE_CLOSE,"Night mode",ACCENT_GOLD,WHITE,false);

    section(test_list,"SCREENS",ACCENT_BLUE);
    g=list_box(test_list,LV_FLEX_FLOW_ROW_WRAP,10);
    test_button(g,SF_TEST_TOUR,LV_SYMBOL_PLAY,"Screen tour",ACCENT_BLUE,WHITE,true);
    test_button(g,SF_TEST_USB,LV_SYMBOL_USB,"USB transfer",ACCENT_BLUE,WHITE,false);
    test_button(g,SF_TEST_WIFI,LV_SYMBOL_WIFI,"Wi-Fi problem",ACCENT_BLUE,WHITE,false);
    test_button(g,SF_TEST_TODAY,LV_SYMBOL_LIST,"Today's sky",ACCENT_BLUE,WHITE,false);
    test_button(g,SF_TEST_SPLASH,LV_SYMBOL_POWER,"Startup",ACCENT_BLUE,WHITE,false);
    test_button(g,SF_TEST_SATELLITE,LV_SYMBOL_GPS,"Satellite radar",ACCENT_BLUE,WHITE,false);
    test_button(g,SF_TEST_RANGES,LV_SYMBOL_REFRESH,"Radar ranges",ACCENT_BLUE,WHITE,false);

    section(test_list,"ALERTS",lv_color_hex(0xFF9F43));
    g=list_box(test_list,LV_FLEX_FLOW_ROW_WRAP,10);
    test_button(g,SF_TEST_WX_ALERT,LV_SYMBOL_WARNING,"Weather alert",lv_color_hex(0xFF9F43),WHITE,false);
    test_button(g,SF_TEST_SIGHTING,LV_SYMBOL_EYE_OPEN,"Sighting alert",lv_color_hex(0xFF9F43),WHITE,false);
    test_button(g,SF_TEST_OVERHEAD,LV_SYMBOL_UP,"Overhead now",lv_color_hex(0xFF9F43),WHITE,false);
    test_button(g,SF_TEST_ISS,LV_SYMBOL_GPS,"Space station",lv_color_hex(0xFF9F43),WHITE,false);

    section(test_list,"SYSTEM",ACCENT_GREEN);
    g=list_box(test_list,LV_FLEX_FLOW_ROW,10);
    static const char *const tile_names[TILE_COUNT]={"CPU","FREE RAM","CHIP"};
    for(int i=0;i<TILE_COUNT;i++){
        lv_obj_t *t=lv_obj_create(g);plain_obj(t);
        lv_obj_set_size(t,(LIST_W-20)/3,62);
        lv_obj_set_style_radius(t,14,0);
        lv_obj_set_style_bg_color(t,WHITE,0);lv_obj_set_style_bg_opa(t,LV_OPA_10,0);
        plain_label(t,tile_names[i],MUTED,&lv_font_montserrat_12,0,8,(LIST_W-20)/3);
        tile_value[i]=plain_label(t,"--",WHITE,&lv_font_montserrat_20,0,28,(LIST_W-20)/3);
    }
    lv_obj_t *c=card(test_list);
    static const char *const row_names[ROW_COUNT]={
        [ROW_RAM_LOW]="Lowest free RAM",[ROW_BLOCK]="Largest RAM block",[ROW_PSRAM]="PSRAM",
        [ROW_SD]="microSD",[ROW_BATTERY]="Battery",[ROW_WIFI]="Wi-Fi",[ROW_IP]="IP address",
        [ROW_UPTIME]="Running for",[ROW_BOOTS]="Times started",[ROW_RESET]="Last restart",
        [ROW_CRASH]="Last crash",[ROW_CLOCK]="Processor",[ROW_VERSION]="Built",[ROW_UPDATE]="Updates",[ROW_ISS]="Space station"};
    for(int i=0;i<ROW_COUNT;i++)row_value[i]=stat_row(c,row_names[i],WHITE);

    section(test_list,"WEB PAGES",ACCENT_GREEN);
    c=card(test_list);
    lv_obj_t *v=stat_row(c,"Address",ACCENT_GREEN);   /* the other pages are linked from it */
    lv_label_set_text(v,"skyframe.local");
    web_row=lv_obj_get_parent(v);
    web_note=text_label(c,"Not on the home Wi-Fi yet. Join the SkyFrame setup network and open http://192.168.4.1",
                        MUTED,LV_FONT_DEFAULT);
    lv_obj_set_width(web_note,LV_PCT(100));
    lv_obj_set_style_text_align(web_note,LV_TEXT_ALIGN_CENTER,0);

    section(test_list,"DEVICE",lv_color_hex(0xFF8A80));
    update_tapped=false;
    update_button_label=test_button(test_list,SF_TEST_UPDATE,LV_SYMBOL_DOWNLOAD,"Check for updates",ACCENT_GREEN,ACCENT_GREEN,true);
    test_button(test_list,SF_TEST_RESTART,LV_SYMBOL_REFRESH,"Restart SkyFrame",lv_color_hex(0xFF8A80),
                lv_color_hex(0xFF6B6B),true);

    /* y 398-446: Exit, always on screen. */
    lv_obj_t *exit=lv_btn_create(test_root);
    lv_obj_set_size(exit,200,48);lv_obj_set_pos(exit,140,398);
    lv_obj_set_style_radius(exit,24,0);
    lv_obj_set_style_shadow_width(exit,0,0);
    lv_obj_set_style_bg_color(exit,ACCENT_GREEN,0);
    lv_obj_add_event_cb(exit,test_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)SF_TEST_EXIT);
    lv_obj_center(text_label(exit,LV_SYMBOL_LEFT "  Exit test mode",lv_color_hex(0x06150D),&lv_font_montserrat_16));
    apply_stats();
}
void sf_test_menu_show(bool show){
    if(show==(test_root!=NULL))return;
    if(show){if(screens_parent){sf_lvmem_prefer_psram(true);build_test_menu();sf_lvmem_prefer_psram(false);
                                lv_obj_clear_flag(test_root,LV_OBJ_FLAG_HIDDEN);}}
    else{lv_obj_del_async(test_root);test_root=NULL;update_button_label=NULL;update_tapped=false;}
}
static void apply_stats(void){
    if(!test_root||!have_test_stats)return;
    const sf_system_stats_t *s=&test_stats;
    char b[64];
    if(s->cpu[0]>=0)snprintf(b,sizeof(b),"%d/%d%%",s->cpu[0],s->cpu[1]); else snprintf(b,sizeof(b),"...");
    set_text(tile_value[TILE_CPU],b);
    snprintf(b,sizeof(b),"%u KB",s->ram_kb);
    set_text(tile_value[TILE_RAM],b);
    if(isfinite(s->chip_c))snprintf(b,sizeof(b),"%.0f" DEG "F",s->chip_c*1.8f+32.f); else snprintf(b,sizeof(b),"--");
    set_text(tile_value[TILE_TEMP],b);

    snprintf(b,sizeof(b),"%u KB",s->ram_low_kb);
    set_text(row_value[ROW_RAM_LOW],b);
    snprintf(b,sizeof(b),"%u KB",s->ram_block_kb);
    set_text(row_value[ROW_BLOCK],b);
    snprintf(b,sizeof(b),"%.1f of %.1f MB free",s->psram_free_mb,s->psram_total_mb);
    set_text(row_value[ROW_PSRAM],b);
    if(isfinite(s->sd_total_gb))snprintf(b,sizeof(b),"%.1f of %.1f GB free",s->sd_free_gb,s->sd_total_gb);
    else snprintf(b,sizeof(b),"Not available");
    set_text(row_value[ROW_SD],b);
    if(isfinite(s->battery_v))snprintf(b,sizeof(b),"%.2f V",s->battery_v); else snprintf(b,sizeof(b),"--");
    set_text(row_value[ROW_BATTERY],b);
    if(s->rssi)snprintf(b,sizeof(b),"%d dBm  \xE2\x80\xA2  channel %d",s->rssi,s->channel);
    else snprintf(b,sizeof(b),"Not connected");
    set_text(row_value[ROW_WIFI],b);
    set_text(row_value[ROW_IP],s->ip[0]?s->ip:"--");
    uint32_t up=s->uptime_s;
    snprintf(b,sizeof(b),"%ud %uh %um",(unsigned)(up/86400),(unsigned)(up/3600%24),(unsigned)(up/60%60));
    set_text(row_value[ROW_UPTIME],b);
    if(s->boots)snprintf(b,sizeof(b),"%u",(unsigned)s->boots); else snprintf(b,sizeof(b),"--");
    set_text(row_value[ROW_BOOTS],b);
    set_text(row_value[ROW_RESET],s->reset_reason?s->reset_reason:"--");
    snprintf(b,sizeof(b),"%u MHz  \xE2\x80\xA2  2 cores",s->cpu_mhz);
    set_text(row_value[ROW_CLOCK],b);
    set_text(row_value[ROW_VERSION],s->built[0]?s->built:"--");
    set_text(row_value[ROW_UPDATE],s->update[0]?s->update:"--");
    if(update_tapped&&update_button_label&&s->update[0])set_text(update_button_label,s->update);
    set_text(row_value[ROW_CRASH],s->crash[0]?s->crash:"None saved");
    set_text(row_value[ROW_ISS],s->iss[0]?s->iss:"--");
    set_text(test_version,s->version);

    /* Web pages: the address on the home Wi-Fi, otherwise how to reach setup. */
    if(s->ip[0]){lv_obj_clear_flag(web_row,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(web_note,LV_OBJ_FLAG_HIDDEN);}
    else{lv_obj_add_flag(web_row,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(web_note,LV_OBJ_FLAG_HIDDEN);}
}
void sf_test_menu_set_stats(const sf_system_stats_t *stats){
    if(!stats)return;
    test_stats=*stats;have_test_stats=true;
    apply_stats();
}
void sf_screens_set_test_cb(void (*enter)(void),void (*action)(sf_test_action_t)){on_test_enter=enter;on_test_action=action;}
static void badge_clicked(lv_event_t *e){
    (void)e;
    if(on_test_action)on_test_action(SF_TEST_STOP);
}
/* "Next birthday" pill under the birthday page while its test runs. */
static lv_obj_t *test_next_btn;
static void test_next_clicked(lv_event_t *e){(void)e;if(on_test_action)on_test_action(SF_TEST_NEXT);}
void sf_test_next(bool show){
    if(!test_next_btn){
        if(!show)return;
        test_next_btn=lv_btn_create(lv_layer_top());
        lv_obj_set_size(test_next_btn,150,34);lv_obj_align(test_next_btn,LV_ALIGN_BOTTOM_MID,0,-24);
        lv_obj_set_style_radius(test_next_btn,17,0);lv_obj_set_style_shadow_width(test_next_btn,0,0);
        lv_obj_set_style_bg_color(test_next_btn,ACCENT_GOLD,0);
        lv_obj_add_event_cb(test_next_btn,test_next_clicked,LV_EVENT_CLICKED,NULL);
        lv_obj_center(text_label(test_next_btn,"Next birthday  " LV_SYMBOL_RIGHT,lv_color_hex(0x1A1200),&lv_font_montserrat_12));
    }
    if(show==lv_obj_has_flag(test_next_btn,LV_OBJ_FLAG_HIDDEN)){
        if(show)lv_obj_clear_flag(test_next_btn,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(test_next_btn,LV_OBJ_FLAG_HIDDEN);
    }
}
void sf_test_badge(const char *text){
    if(!test_badge){
        test_badge=lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(test_badge,ACCENT_GOLD,0);
        lv_obj_set_style_bg_opa(test_badge,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(test_badge,lv_color_hex(0x1A1200),0);
        lv_obj_set_style_text_font(test_badge,&lv_font_montserrat_12,0);
        lv_obj_set_style_radius(test_badge,13,0);
        lv_obj_set_style_pad_hor(test_badge,12,0);
        lv_obj_set_style_pad_ver(test_badge,6,0);
        lv_obj_align(test_badge,LV_ALIGN_TOP_MID,0,24);
        /* Tap to stop the test; a generous touch area around the small pill. */
        lv_obj_add_flag(test_badge,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(test_badge,16);
        lv_obj_add_event_cb(test_badge,badge_clicked,LV_EVENT_CLICKED,NULL);
        lv_obj_add_flag(test_badge,LV_OBJ_FLAG_HIDDEN);
    }
    if(text){
        char b[96];
        snprintf(b,sizeof(b),"%s   " LV_SYMBOL_CLOSE " Stop",text);
        if(strcmp(lv_label_get_text(test_badge),b))lv_label_set_text(test_badge,b);
        if(lv_obj_has_flag(test_badge,LV_OBJ_FLAG_HIDDEN))lv_obj_clear_flag(test_badge,LV_OBJ_FLAG_HIDDEN);
    }else if(!lv_obj_has_flag(test_badge,LV_OBJ_FLAG_HIDDEN))lv_obj_add_flag(test_badge,LV_OBJ_FLAG_HIDDEN);
}


/* ---- Today's sky ------------------------------------------------------------
   In the style of the details page and the weather chips:
     y 34-66    TODAY'S SKY and the date
     y 76-232   a photo of the day's rarest aircraft, its name over a dark fade at the bottom
     y 242-298  the day's count
     y 306-366  three glass chips: highest, closest, fastest
     y 378-404  planes per hour as a slim bar strip, the current hour in green
     y 412      VIP / military flights, if any */
#define TODAY_PHOTO_X 100
#define TODAY_PHOTO_Y 76
#define TODAY_BAR_X 145      /* 24 bars of 6 px, 2 px apart: 190 px */
#define TODAY_BAR_BOTTOM 404
#define TODAY_BAR_MAX 24
enum { TT_HIGH, TT_CLOSE, TT_FAST, TT_COUNT };
static lv_obj_t *today_root, *today_date, *today_count, *today_noun, *today_bar[24];
static lv_obj_t *today_value[TT_COUNT], *today_who[TT_COUNT], *today_special;
static lv_obj_t *today_card, *today_img, *today_placeholder, *today_hero_tag, *today_hero_name, *today_hero_who;
static lv_img_dsc_t today_dsc;
static uint16_t *today_pixels;      /* the hero photo; owned here */
static void build_today(lv_obj_t *parent){
    today_root=screen(parent,lv_color_hex(0x10213D));
    lv_obj_set_style_bg_grad_color(today_root,lv_color_hex(0x060B17),0);
    lv_obj_set_style_bg_grad_dir(today_root,LV_GRAD_DIR_VER,0);
    sf_gesture_attach(today_root);
    lv_obj_t *t=plain_label(today_root,"TODAY'S SKY",ACCENT_GREEN,&lv_font_montserrat_14,120,33,240);
    lv_obj_set_style_text_letter_space(t,3,0);
    today_date=plain_label(today_root,"",MUTED,LV_FONT_DEFAULT,110,52,260);

    /* Hero card: rounded photo with its caption over a dark fade. */
    today_card=lv_obj_create(today_root);plain_obj(today_card);
    lv_obj_set_pos(today_card,TODAY_PHOTO_X,TODAY_PHOTO_Y);lv_obj_set_size(today_card,280,156);
    lv_obj_set_style_radius(today_card,18,0);lv_obj_set_style_clip_corner(today_card,true,0);
    lv_obj_set_style_bg_color(today_card,lv_color_hex(0x1D3A66),0);
    lv_obj_set_style_bg_grad_color(today_card,lv_color_hex(0x0C1830),0);
    lv_obj_set_style_bg_grad_dir(today_card,LV_GRAD_DIR_VER,0);
    lv_obj_set_style_bg_opa(today_card,LV_OPA_COVER,0);
    today_placeholder=drawn_icon(today_card,ICON_PLANE,76);   /* until a photo arrives, or none exists */
    lv_obj_align(today_placeholder,LV_ALIGN_CENTER,0,-18);
    today_img=lv_img_create(today_card);lv_obj_set_pos(today_img,0,0);
    lv_obj_add_flag(today_img,LV_OBJ_FLAG_HIDDEN);
    for(int i=0;i<16;i++){   /* opacity can't be a gradient here: 16 thin bands, 4% to 64% */
        lv_obj_t *b=lv_obj_create(today_card);plain_obj(b);
        lv_obj_set_pos(b,0,84+i*5);lv_obj_set_size(b,280,i==15?72-15*5:5);
        lv_obj_set_style_bg_color(b,lv_color_hex(0x000000),0);lv_obj_set_style_bg_opa(b,(lv_opa_t)(10+i*10),0);
    }
    today_hero_tag=text_label(today_card,"RAREST TODAY",ACCENT_GOLD,&lv_font_montserrat_12);
    lv_obj_set_style_text_letter_space(today_hero_tag,2,0);
    lv_obj_set_pos(today_hero_tag,16,104);
    today_hero_name=text_label(today_card,"",WHITE,&lv_font_montserrat_16);
    lv_label_set_long_mode(today_hero_name,LV_LABEL_LONG_DOT);
    lv_obj_set_width(today_hero_name,170);lv_obj_set_pos(today_hero_name,16,124);
    today_hero_who=text_label(today_card,"",MUTED,&lv_font_montserrat_12);
    lv_label_set_long_mode(today_hero_who,LV_LABEL_LONG_DOT);
    lv_obj_set_width(today_hero_who,84);lv_obj_set_style_text_align(today_hero_who,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_pos(today_hero_who,180,127);

    /* The count, with its words beside it. */
    lv_obj_t *row=lv_obj_create(today_root);plain_obj(row);
    lv_obj_set_size(row,480,56);lv_obj_set_pos(row,0,242);
    lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row,12,0);
    today_count=text_label(row,"0",WHITE,&lv_font_montserrat_48);
    today_noun=text_label(row,"planes\nspotted today",MUTED,LV_FONT_DEFAULT);

    static const char *const names[TT_COUNT]={"HIGHEST","CLOSEST","FASTEST"};
    for(int i=0;i<TT_COUNT;i++){
        lv_obj_t *chip=lv_obj_create(today_root);plain_obj(chip);
        lv_obj_set_pos(chip,76+i*112,306);lv_obj_set_size(chip,104,60);
        lv_obj_set_style_radius(chip,14,0);
        lv_obj_set_style_bg_color(chip,WHITE,0);lv_obj_set_style_bg_opa(chip,LV_OPA_10,0);
        plain_label(chip,names[i],MUTED,&lv_font_montserrat_12,0,6,104);
        today_value[i]=plain_label(chip,"--",WHITE,&lv_font_montserrat_16,0,22,104);
        today_who[i]=plain_label(chip,"",ACCENT_GREEN,&lv_font_montserrat_12,4,42,96);
        lv_label_set_long_mode(today_who[i],LV_LABEL_LONG_DOT);
        lv_obj_set_height(today_who[i],lv_font_get_line_height(&lv_font_montserrat_12));
    }
    for(int h=0;h<24;h++){
        today_bar[h]=lv_obj_create(today_root);plain_obj(today_bar[h]);
        lv_obj_set_style_radius(today_bar[h],2,0);
        lv_obj_set_style_bg_opa(today_bar[h],LV_OPA_COVER,0);
        lv_obj_set_size(today_bar[h],6,2);
        lv_obj_set_pos(today_bar[h],TODAY_BAR_X+h*8,TODAY_BAR_BOTTOM-2);
    }
    today_special=plain_label(today_root,"",lv_color_hex(0xC89BFF),&lv_font_montserrat_12,90,412,300);
    lv_label_set_long_mode(today_special,LV_LABEL_LONG_DOT);
    lv_obj_set_height(today_special,lv_font_get_line_height(&lv_font_montserrat_12));
}
static void set_or_hide(lv_obj_t *label,const char *s){
    if(s&&s[0]){set_text(label,s);lv_obj_clear_flag(label,LV_OBJ_FLAG_HIDDEN);}
    else lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
}
bool sf_today_set_photo(uint16_t *pixels){
    if(!today_root)return false;
    uint16_t *old=today_pixels;
    today_pixels=pixels;
    if(pixels){
        memset(&today_dsc,0,sizeof(today_dsc));
        today_dsc.header.cf=LV_IMG_CF_TRUE_COLOR;today_dsc.header.w=280;today_dsc.header.h=156;
        today_dsc.data_size=280u*156u*2u;today_dsc.data=(const uint8_t*)pixels;
        lv_img_cache_invalidate_src(&today_dsc);
        lv_img_set_src(today_img,&today_dsc);
        lv_obj_clear_flag(today_img,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(today_placeholder,LV_OBJ_FLAG_HIDDEN);
    }else{
        lv_obj_add_flag(today_img,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(today_placeholder,LV_OBJ_FLAG_HIDDEN);
    }
    free(old);   /* the image no longer points at it */
    return true;
}
void sf_today_set(const sf_today_view_t *v){
    if(!today_root||!v)return;
    char b[32];
    set_text(today_date,v->date?v->date:"");
    snprintf(b,sizeof(b),"%u",v->planes);
    set_text(today_count,b);
    set_text(today_noun,v->planes==1?"plane\nspotted today":"planes\nspotted today");
    set_text(today_hero_tag,v->hero_tag&&v->hero_tag[0]?v->hero_tag:"RAREST TODAY");
    set_text(today_hero_name,v->hero_name&&v->hero_name[0]?v->hero_name:"Watching the sky...");
    set_text(today_hero_who,v->hero_who?v->hero_who:"");
    const char *values[TT_COUNT]={v->highest,v->closest,v->fastest};
    const char *who[TT_COUNT]={v->highest_who,v->closest_who,v->fastest_who};
    for(int i=0;i<TT_COUNT;i++){
        set_text(today_value[i],values[i]&&values[i][0]?values[i]:"--");
        set_text(today_who[i],who[i]?who[i]:"");
    }
    unsigned most=1;
    for(int h=0;h<24;h++)if(v->per_hour[h]>most)most=v->per_hour[h];
    for(int h=0;h<24;h++){
        int height=v->per_hour[h]?4+(int)((TODAY_BAR_MAX-4)*v->per_hour[h]/most):2;
        lv_obj_set_height(today_bar[h],height);
        lv_obj_set_y(today_bar[h],TODAY_BAR_BOTTOM-height);
        bool now=h==v->now_hour;
        lv_obj_set_style_bg_color(today_bar[h],now?ACCENT_GREEN:ACCENT_BLUE,0);
        lv_obj_set_style_bg_opa(today_bar[h],now?LV_OPA_COVER:v->per_hour[h]?LV_OPA_70:LV_OPA_20,0);
    }
    /* Under the bars: VIP and military flights if any, otherwise the busiest hour so far. */
    char sp[160]="";
    if(v->special&&v->special[0]){
        snprintf(sp,sizeof(sp),"%s%s%s",v->special,v->special_who&&v->special_who[0]?":  ":"",v->special_who?v->special_who:"");
        lv_obj_set_style_text_color(today_special,lv_color_hex(0xC89BFF),0);
    }else if(v->planes){
        int busiest=0;
        for(int h=1;h<24;h++)if(v->per_hour[h]>v->per_hour[busiest])busiest=h;
        snprintf(sp,sizeof(sp),"Busiest hour  %d %s  \xE2\x80\xA2  %u planes",busiest%12?busiest%12:12,busiest<12?"AM":"PM",
                 (unsigned)v->per_hour[busiest]);
        lv_obj_set_style_text_color(today_special,FAINT,0);
    }
    set_or_hide(today_special,sp);
}
void sf_today_show(bool show){
    if(!today_root)return;
    if(show)lv_obj_clear_flag(today_root,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(today_root,LV_OBJ_FLAG_HIDDEN);
}

/* ---- Sighting alert ---------------------------------------------------------
   On the top layer, so it shows over whichever screen is up. The rim ring pulses three
   times as it arrives; the banner's glow keeps breathing until it leaves. */
static const uint32_t sighting_color[]={0x7CF5A3,0x6EDCFF,0xFFC857,0xC89BFF,0xFF5A5A,0x6EB6FF,0xE8EEFF};   /* overhead, type, military, VIP, emergency, news, ISS */
static lv_obj_t *sight_ring, *sight_banner, *sight_title, *sight_name, *sight_detail;
static lv_timer_t *sight_timer;
static void (*on_sighting_tap)(void);
static bool sight_is_news;   /* the "Updated to" card: a tap only closes it */
void sf_screens_set_sighting_cb(void (*tapped)(void)){on_sighting_tap=tapped;}
static lv_opa_t ring_opa;
static lv_color_t ring_color;
#define RING_W 14
static void ring_draw(lv_event_t *e){
    if(!ring_opa)return;
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
    d.color=ring_color;d.width=RING_W;d.opa=ring_opa;
    lv_point_t c={240,240};
    lv_draw_arc(lv_event_get_draw_ctx(e),&d,&c,240,0,360);
}
/* The ring only: 16 boxes around the rim instead of the whole screen (under the radar's
   sweep, a full-screen redraw every frame is costly). */
static void anim_ring_opa(void *o,int32_t v){
    ring_opa=(lv_opa_t)v;
    for(int k=0;k<16;k++){
        float a0=(float)k*22.5f*3.14159265f/180.f,a1=(float)(k+1)*22.5f*3.14159265f/180.f;
        float xs[4]={240+cosf(a0)*240,240+cosf(a1)*240,240+cosf(a0)*(240-RING_W),240+cosf(a1)*(240-RING_W)};
        float ys[4]={240+sinf(a0)*240,240+sinf(a1)*240,240+sinf(a0)*(240-RING_W),240+sinf(a1)*(240-RING_W)};
        lv_area_t b={480,480,0,0};
        for(int i=0;i<4;i++){
            if(xs[i]<b.x1)b.x1=(lv_coord_t)floorf(xs[i]);
            if(xs[i]>b.x2)b.x2=(lv_coord_t)ceilf(xs[i]);
            if(ys[i]<b.y1)b.y1=(lv_coord_t)floorf(ys[i]);
            if(ys[i]>b.y2)b.y2=(lv_coord_t)ceilf(ys[i]);
        }
        lv_area_increase(&b,2,2);
        lv_obj_invalidate_area((lv_obj_t*)o,&b);
    }
}
static void anim_shadow_opa(void *o,int32_t v){lv_obj_set_style_shadow_opa((lv_obj_t*)o,(lv_opa_t)v,0);}
static void hide_when_done(lv_anim_t *a){lv_obj_add_flag((lv_obj_t*)a->var,LV_OBJ_FLAG_HIDDEN);}
static void sighting_hide(bool animated){
    if(sight_timer){lv_timer_del(sight_timer);sight_timer=NULL;}
    if(!sight_banner)return;
    lv_anim_del(sight_ring,NULL);
    lv_obj_add_flag(sight_ring,LV_OBJ_FLAG_HIDDEN);
    lv_anim_del(sight_banner,NULL);
    if(lv_obj_has_flag(sight_banner,LV_OBJ_FLAG_HIDDEN))return;
    lv_obj_clear_flag(sight_banner,LV_OBJ_FLAG_CLICKABLE);   /* leaving: no second tap */
    if(!animated){lv_obj_add_flag(sight_banner,LV_OBJ_FLAG_HIDDEN);return;}
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,sight_banner);lv_anim_set_exec_cb(&a,anim_y);
    lv_anim_set_values(&a,lv_obj_get_y(sight_banner),-130);lv_anim_set_time(&a,350);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_in);lv_anim_set_ready_cb(&a,hide_when_done);
    lv_anim_start(&a);
}
static void sighting_timeout(lv_timer_t *t){(void)t;sight_timer=NULL;sighting_hide(true);}
static void sighting_clicked(lv_event_t *e){
    (void)e;
    sighting_hide(true);
    if(on_sighting_tap&&!sight_is_news)on_sighting_tap();
}
static void build_sighting(void){
    lv_obj_t *top=lv_layer_top();
    sight_ring=lv_obj_create(top);plain_obj(sight_ring);
    lv_obj_set_size(sight_ring,480,480);
    lv_obj_set_style_radius(sight_ring,LV_RADIUS_CIRCLE,0);
    lv_obj_add_event_cb(sight_ring,ring_draw,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_add_flag(sight_ring,LV_OBJ_FLAG_HIDDEN);
    sight_banner=lv_obj_create(top);plain_obj(sight_banner);
    lv_obj_set_size(sight_banner,340,100);lv_obj_set_pos(sight_banner,70,-130);
    lv_obj_set_style_radius(sight_banner,26,0);
    lv_obj_set_style_bg_color(sight_banner,lv_color_hex(0x0A1222),0);lv_obj_set_style_bg_opa(sight_banner,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(sight_banner,3,0);
    lv_obj_set_style_shadow_width(sight_banner,28,0);
    lv_obj_set_ext_click_area(sight_banner,10);
    lv_obj_add_event_cb(sight_banner,sighting_clicked,LV_EVENT_CLICKED,NULL);
    sight_title=plain_label(sight_banner,"",WHITE,&lv_font_montserrat_20,10,12,320);
    sight_name=plain_label(sight_banner,"",WHITE,&lv_font_montserrat_16,10,42,320);
    sight_detail=plain_label(sight_banner,"",MUTED,&lv_font_montserrat_12,10,70,320);
    lv_obj_t *labels[]={sight_title,sight_name,sight_detail};
    for(int i=0;i<3;i++){
        lv_label_set_long_mode(labels[i],LV_LABEL_LONG_DOT);
        lv_obj_set_height(labels[i],lv_font_get_line_height(lv_obj_get_style_text_font(labels[i],0)));
    }
    lv_obj_add_flag(sight_banner,LV_OBJ_FLAG_HIDDEN);
}
bool sf_sighting_visible(void){return sight_banner&&!lv_obj_has_flag(sight_banner,LV_OBJ_FLAG_HIDDEN);}
void sf_sighting_show(const sf_sighting_view_t *v){
    if(!v){sighting_hide(false);return;}
    if(!sight_banner)build_sighting();
    sighting_hide(false);
    sight_is_news=v->kind==SF_SIGHTING_NEWS||v->kind==SF_SIGHTING_ISS;   /* a tap only closes these */
    lv_color_t c=lv_color_hex(sighting_color[v->kind<=SF_SIGHTING_ISS?v->kind:0]);
    char b[96];
    snprintf(b,sizeof(b),"%s  %s",v->kind==SF_SIGHTING_EMERGENCY?LV_SYMBOL_WARNING:v->kind==SF_SIGHTING_OVERHEAD?LV_SYMBOL_UP:
             v->kind==SF_SIGHTING_NEWS?LV_SYMBOL_DOWNLOAD:v->kind==SF_SIGHTING_ISS?LV_SYMBOL_GPS:LV_SYMBOL_EYE_OPEN,
             v->title?v->title:"");
    lv_label_set_text(sight_title,b);
    lv_obj_set_style_text_color(sight_title,c,0);
    lv_label_set_text(sight_name,v->name?v->name:"");
    if(sight_is_news)snprintf(b,sizeof(b),"%s",v->detail?v->detail:"");
    else snprintf(b,sizeof(b),"%s%sTap to see it on the radar",v->detail?v->detail:"",v->detail&&v->detail[0]?"  \xE2\x80\xA2  ":"");
    lv_label_set_text(sight_detail,b);
    lv_obj_set_style_border_color(sight_banner,c,0);
    lv_obj_set_style_shadow_color(sight_banner,c,0);
    lv_obj_add_flag(sight_banner,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_y(sight_banner,-130);
    lv_obj_clear_flag(sight_banner,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sight_ring);
    lv_obj_move_foreground(sight_banner);
    lv_anim_t a;lv_anim_init(&a);   /* slide in */
    lv_anim_set_var(&a,sight_banner);lv_anim_set_exec_cb(&a,anim_y);
    lv_anim_set_values(&a,-130,64);lv_anim_set_time(&a,420);lv_anim_set_path_cb(&a,lv_anim_path_overshoot);
    lv_anim_start(&a);
    lv_anim_init(&a);               /* glow, breathing until it leaves */
    lv_anim_set_var(&a,sight_banner);lv_anim_set_exec_cb(&a,anim_shadow_opa);
    lv_anim_set_values(&a,LV_OPA_20,LV_OPA_80);lv_anim_set_time(&a,800);lv_anim_set_playback_time(&a,800);
    lv_anim_set_repeat_count(&a,LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    ring_color=c;ring_opa=LV_OPA_TRANSP;   /* rim: three pulses (one for "overhead") */
    lv_obj_clear_flag(sight_ring,LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&a);
    lv_anim_set_var(&a,sight_ring);lv_anim_set_exec_cb(&a,anim_ring_opa);
    lv_anim_set_values(&a,LV_OPA_TRANSP,LV_OPA_90);lv_anim_set_time(&a,450);lv_anim_set_playback_time(&a,450);
    lv_anim_set_repeat_count(&a,v->kind==SF_SIGHTING_OVERHEAD||sight_is_news?1:3);lv_anim_set_ready_cb(&a,hide_when_done);
    lv_anim_start(&a);
    sight_timer=lv_timer_create(sighting_timeout,(v->seconds?v->seconds:20)*1000u,NULL);
    lv_timer_set_repeat_count(sight_timer,1);
}

/* ---- Pull-down panel ----------------------------------------------------------
   Slides down over the top of the screen on the top layer (the screen underneath keeps
   running). Panel coordinates are screen coordinates once it is down:
   y 34 status, 58-96 brightness, 116-190 screen shortcuts, 204-248 pause and settings,
   276 handle. Built on first use, then kept (hidden). */
#define QUICK_H 420
#define QUICK_IDLE_MS 20000
#define QUICK_GOLD lv_color_hex(0xFFC857)
static lv_obj_t *quick_back, *quick_panel, *quick_status, *quick_bright_label, *quick_slider;
static lv_obj_t *quick_btn[4], *quick_pause_btn, *quick_pause_label, *quick_usb_btn, *quick_anim_switch, *quick_sat_switch;
static lv_obj_t *quick_range_btn, *quick_range_label;   /* "10 mi": tap for the next range */
static lv_timer_t *quick_timer;
static void (*on_quick_brightness)(int, bool);
static void (*on_quick_action)(sf_quick_action_t);
static sf_quick_state_t quick_state={.brightness=100,.screen=SF_SCREEN_RADAR};
static char quick_status_text[64];
static uint32_t quick_hint_at;            /* a hint replaces the status line for a few seconds */
void sf_quick_set_callbacks(void (*brightness)(int,bool),void (*action)(sf_quick_action_t)){
    on_quick_brightness=brightness;on_quick_action=action;
}
bool sf_quick_is_open(void){return quick_panel&&!lv_obj_has_flag(quick_panel,LV_OBJ_FLAG_HIDDEN);}
static void quick_hidden(lv_anim_t *a){(void)a;lv_obj_add_flag(quick_panel,LV_OBJ_FLAG_HIDDEN);}
void sf_quick_close(void){
    if(!sf_quick_is_open()||lv_obj_has_flag(quick_back,LV_OBJ_FLAG_HIDDEN))return;   /* closed or closing */
    lv_obj_add_flag(quick_back,LV_OBJ_FLAG_HIDDEN);
    if(quick_timer)lv_timer_pause(quick_timer);
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,quick_panel);lv_anim_set_exec_cb(&a,anim_y);
    lv_anim_set_values(&a,lv_obj_get_y(quick_panel),-QUICK_H-30);lv_anim_set_time(&a,260);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_in);lv_anim_set_ready_cb(&a,quick_hidden);
    lv_anim_start(&a);
}
static void quick_idle(lv_timer_t *t){(void)t;if(lv_disp_get_inactive_time(NULL)>=QUICK_IDLE_MS)sf_quick_close();}
static void quick_back_pressed(lv_event_t *e){(void)e;sf_quick_close();}
static void quick_panel_gesture(lv_event_t *e){
    (void)e;
    if(lv_indev_get_gesture_dir(lv_indev_get_act())==LV_DIR_TOP)sf_quick_close();
}
static void quick_clicked(lv_event_t *e){
    sf_quick_action_t a=(sf_quick_action_t)(uintptr_t)lv_event_get_user_data(e);
    if(a==SF_QUICK_TODAY&&!quick_state.today_available)return;
    if(a==SF_QUICK_USB&&!quick_state.usb_available){   /* no computer on the USB port: say so */
        snprintf(quick_status_text,sizeof(quick_status_text),"Connect the USB port to a computer first");
        set_text(quick_status,quick_status_text);
        quick_hint_at=lv_tick_get();
        return;
    }
    if(on_quick_action)on_quick_action(a);
    if(a!=SF_QUICK_PAUSE&&a!=SF_QUICK_RANGE)sf_quick_close();   /* went somewhere: out of the way */
}
static void quick_sat_switched(lv_event_t *e){
    (void)e;
    quick_state.satellite=lv_obj_has_state(quick_sat_switch,LV_STATE_CHECKED);
    if(on_quick_action)on_quick_action(SF_QUICK_SATELLITE);
}
static lv_obj_t *quick_switch_row(int y,const char *text,lv_event_cb_t cb){
    lv_obj_t *row=lv_obj_create(quick_panel);plain_obj(row);
    lv_obj_set_pos(row,120,y);lv_obj_set_size(row,240,34);
    lv_obj_t *name=text_label(row,text,WHITE,LV_FONT_DEFAULT);
    lv_obj_align(name,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_t *sw=lv_switch_create(row);
    lv_obj_set_size(sw,52,28);
    lv_obj_align(sw,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_set_style_bg_color(sw,lv_color_hex(0x2A3A55),LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw,ACCENT_GREEN,LV_PART_INDICATOR|LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw,8);
    lv_obj_add_event_cb(sw,cb,LV_EVENT_VALUE_CHANGED,NULL);
    return sw;
}
static void quick_switched(lv_event_t *e){
    (void)e;
    quick_state.transitions=lv_obj_has_state(quick_anim_switch,LV_STATE_CHECKED);
    if(on_quick_action)on_quick_action(SF_QUICK_TRANSITIONS);
}
static void quick_slid(lv_event_t *e){
    int v=(int)lv_slider_get_value(quick_slider);
    bool done=lv_event_get_code(e)==LV_EVENT_RELEASED;
    char b[32];snprintf(b,sizeof(b),"Brightness  %d%%",v);
    set_text(quick_bright_label,b);
    quick_state.brightness=v;
    if(on_quick_brightness)on_quick_brightness(v,done);
}
static lv_obj_t *quick_disc(lv_obj_t *parent,int x,int y,int d,lv_color_t c){
    lv_obj_t *o=lv_obj_create(parent);plain_obj(o);
    lv_obj_set_size(o,d,d);lv_obj_set_pos(o,x,y);
    lv_obj_set_style_radius(o,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(o,c,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    return o;
}
static lv_obj_t *quick_pill(lv_obj_t *parent,int x,int y,int w,sf_quick_action_t a){
    lv_obj_t *b=lv_btn_create(parent);
    lv_obj_set_size(b,w,46);lv_obj_set_pos(b,x,y);
    lv_obj_set_style_radius(b,23,0);lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_bg_color(b,WHITE,0);lv_obj_set_style_bg_opa(b,LV_OPA_10,0);
    lv_obj_set_style_bg_opa(b,LV_OPA_30,LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,quick_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)a);
    return b;
}
static void build_quick(void){
    lv_obj_t *top=lv_layer_top();
    /* Below the panel, catches taps to close it; transparent, so it costs nothing to draw. */
    quick_back=lv_obj_create(top);plain_obj(quick_back);
    lv_obj_set_size(quick_back,480,480);
    lv_obj_add_flag(quick_back,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(quick_back,quick_back_pressed,LV_EVENT_PRESSED,NULL);

    quick_panel=lv_obj_create(top);plain_obj(quick_panel);
    lv_obj_set_size(quick_panel,480,QUICK_H);lv_obj_set_pos(quick_panel,0,-QUICK_H-30);
    lv_obj_set_style_radius(quick_panel,48,0);
    lv_obj_set_style_bg_color(quick_panel,lv_color_hex(0x0B1426),0);lv_obj_set_style_bg_opa(quick_panel,LV_OPA_COVER,0);
    lv_obj_set_style_shadow_width(quick_panel,30,0);lv_obj_set_style_shadow_opa(quick_panel,LV_OPA_60,0);
    lv_obj_set_style_shadow_color(quick_panel,lv_color_hex(0x000000),0);
    lv_obj_add_flag(quick_panel,LV_OBJ_FLAG_CLICKABLE);   /* taps between controls stay on the panel */
    lv_obj_clear_flag(quick_panel,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(quick_panel,quick_panel_gesture,LV_EVENT_GESTURE,NULL);

    quick_status=plain_label(quick_panel,"",MUTED,&lv_font_montserrat_12,120,34,240);
    lv_label_set_long_mode(quick_status,LV_LABEL_LONG_DOT);
    lv_obj_set_height(quick_status,lv_font_get_line_height(&lv_font_montserrat_12));

    quick_bright_label=plain_label(quick_panel,"Brightness",WHITE,&lv_font_montserrat_12,140,58,200);
    quick_disc(quick_panel,84,84,8,FAINT);           /* dim end */
    quick_disc(quick_panel,380,80,16,QUICK_GOLD);    /* bright end */
    quick_slider=lv_slider_create(quick_panel);
    lv_obj_set_pos(quick_slider,110,83);lv_obj_set_size(quick_slider,258,10);
    lv_slider_set_range(quick_slider,10,100);
    lv_obj_set_style_bg_color(quick_slider,WHITE,LV_PART_MAIN);lv_obj_set_style_bg_opa(quick_slider,LV_OPA_20,LV_PART_MAIN);
    lv_obj_set_style_bg_color(quick_slider,QUICK_GOLD,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(quick_slider,WHITE,LV_PART_KNOB);
    lv_obj_set_style_pad_all(quick_slider,7,LV_PART_KNOB);
    lv_obj_set_ext_click_area(quick_slider,18);
    lv_obj_clear_flag(quick_slider,LV_OBJ_FLAG_GESTURE_BUBBLE);   /* dragging is not a swipe */
    lv_obj_add_event_cb(quick_slider,quick_slid,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(quick_slider,quick_slid,LV_EVENT_RELEASED,NULL);

    /* Screen shortcuts: round buttons 60 px, 22 apart, captions underneath. */
    static const char *const caps[4]={"Radar","Weather","Today","Photos"};
    static const sf_quick_action_t acts[4]={SF_QUICK_RADAR,SF_QUICK_WEATHER,SF_QUICK_TODAY,SF_QUICK_PHOTOS};
    for(int i=0;i<4;i++){
        int x=87+i*82;
        lv_obj_t *b=lv_btn_create(quick_panel);
        quick_btn[i]=b;
        lv_obj_set_size(b,60,60);lv_obj_set_pos(b,x,112);
        lv_obj_set_style_radius(b,LV_RADIUS_CIRCLE,0);lv_obj_set_style_shadow_width(b,0,0);
        lv_obj_set_style_bg_color(b,WHITE,0);lv_obj_set_style_bg_opa(b,LV_OPA_10,0);
        lv_obj_set_style_bg_opa(b,LV_OPA_30,LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b,ACCENT_GREEN,0);lv_obj_set_style_border_width(b,0,0);
        lv_obj_add_event_cb(b,quick_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)acts[i]);
        if(acts[i]==SF_QUICK_PHOTOS)lv_obj_center(text_label(b,LV_SYMBOL_IMAGE,WHITE,&lv_font_montserrat_20));
        else lv_obj_center(drawn_icon(b,acts[i]==SF_QUICK_RADAR?ICON_RADAR:acts[i]==SF_QUICK_WEATHER?ICON_SUN:ICON_PLANE,28));
        plain_label(quick_panel,caps[i],MUTED,&lv_font_montserrat_12,x-10,176,80);
    }

    /* Pause / USB file transfer / Settings, 100 px each. */
    quick_pause_btn=quick_pill(quick_panel,84,204,100,SF_QUICK_PAUSE);
    quick_pause_label=text_label(quick_pause_btn,"",WHITE,LV_FONT_DEFAULT);
    lv_obj_center(quick_pause_label);
    quick_usb_btn=quick_pill(quick_panel,190,204,100,SF_QUICK_USB);
    lv_obj_center(text_label(quick_usb_btn,LV_SYMBOL_USB "  USB",WHITE,LV_FONT_DEFAULT));
    lv_obj_t *set=quick_pill(quick_panel,296,204,100,SF_QUICK_SETTINGS);
    lv_obj_center(text_label(set,LV_SYMBOL_SETTINGS " Settings",WHITE,LV_FONT_DEFAULT));

    /* y 258-292: page animations (also on the settings page); y 298-332: satellite radar. */
    quick_anim_switch=quick_switch_row(258,"Animations",quick_switched);
    quick_sat_switch=quick_switch_row(298,"Satellite",quick_sat_switched);

    /* y 338-372: radar range; each tap steps 1 -> 5 -> 10 -> 1 mi (the settings page can
       still set any radius). */
    lv_obj_t *rl=text_label(quick_panel,"Range",WHITE,LV_FONT_DEFAULT);
    lv_obj_set_pos(rl,120,347);
    quick_range_btn=quick_pill(quick_panel,282,338,78,SF_QUICK_RANGE);
    lv_obj_set_height(quick_range_btn,34);lv_obj_set_style_radius(quick_range_btn,17,0);
    lv_obj_set_style_pad_all(quick_range_btn,0,0);
    lv_obj_set_style_bg_color(quick_range_btn,ACCENT_GREEN,0);lv_obj_set_style_bg_opa(quick_range_btn,LV_OPA_20,0);
    lv_obj_set_style_bg_opa(quick_range_btn,LV_OPA_40,LV_STATE_PRESSED);
    lv_obj_set_style_border_color(quick_range_btn,ACCENT_GREEN,0);lv_obj_set_style_border_width(quick_range_btn,1,0);
    lv_obj_set_ext_click_area(quick_range_btn,8);
    quick_range_label=text_label(quick_range_btn,"",WHITE,LV_FONT_DEFAULT);
    lv_obj_center(quick_range_label);

    lv_obj_t *handle=lv_obj_create(quick_panel);plain_obj(handle);
    lv_obj_set_size(handle,48,5);lv_obj_set_pos(handle,216,396);
    lv_obj_set_style_radius(handle,3,0);
    lv_obj_set_style_bg_color(handle,WHITE,0);lv_obj_set_style_bg_opa(handle,LV_OPA_40,0);

    lv_obj_add_flag(quick_panel,LV_OBJ_FLAG_HIDDEN);
    quick_timer=lv_timer_create(quick_idle,1000,NULL);
    lv_timer_pause(quick_timer);
}
static void quick_apply(void){
    if(!quick_panel)return;
    const sf_quick_state_t *q=&quick_state;
    set_text(quick_status,quick_status_text);
    if(!lv_obj_has_state(quick_slider,LV_STATE_PRESSED)){   /* never fight a finger on the slider */
        lv_slider_set_value(quick_slider,q->brightness,LV_ANIM_OFF);
        char b[32];snprintf(b,sizeof(b),"Brightness  %d%%",q->brightness);
        set_text(quick_bright_label,b);
    }
    static const sf_screen_t screens[4]={SF_SCREEN_RADAR,SF_SCREEN_WEATHER,SF_SCREEN_TODAY,SF_SCREEN_PHOTOS};
    for(int i=0;i<4;i++){
        lv_obj_set_style_border_width(quick_btn[i],q->screen==screens[i]?2:0,0);
        lv_obj_set_style_opa(quick_btn[i],screens[i]==SF_SCREEN_TODAY&&!q->today_available?LV_OPA_40:LV_OPA_COVER,0);
    }
    lv_obj_set_style_opa(quick_usb_btn,q->usb_available?LV_OPA_COVER:LV_OPA_40,0);
    set_text(quick_pause_label,q->paused?LV_SYMBOL_PLAY "  Resume":LV_SYMBOL_PAUSE "  Pause");
    lv_obj_set_style_bg_color(quick_pause_btn,q->paused?QUICK_GOLD:WHITE,0);
    lv_obj_set_style_bg_opa(quick_pause_btn,q->paused?LV_OPA_40:LV_OPA_10,0);
    if(q->transitions!=lv_obj_has_state(quick_anim_switch,LV_STATE_CHECKED)){
        if(q->transitions)lv_obj_add_state(quick_anim_switch,LV_STATE_CHECKED);
        else lv_obj_clear_state(quick_anim_switch,LV_STATE_CHECKED);
    }
    {
        char b[16];snprintf(b,sizeof(b),"%u mi",q->range);
        set_text(quick_range_label,b);
    }
    if(q->satellite!=lv_obj_has_state(quick_sat_switch,LV_STATE_CHECKED)){
        if(q->satellite)lv_obj_add_state(quick_sat_switch,LV_STATE_CHECKED);
        else lv_obj_clear_state(quick_sat_switch,LV_STATE_CHECKED);
    }
}
void sf_quick_update(const sf_quick_state_t *state){
    if(!state)return;
    /* Restyling redraws the panel even with equal values: skip when nothing changed. */
    const sf_quick_state_t *q=&quick_state;
    bool same=q->brightness==state->brightness&&q->paused==state->paused&&q->today_available==state->today_available&&
              q->usb_available==state->usb_available&&q->transitions==state->transitions&&q->satellite==state->satellite&&q->range==state->range&&
              q->screen==state->screen&&
              !quick_hint_at&&!strcmp(quick_status_text,state->status?state->status:"");
    if(same)return;
    quick_state=*state;
    if(!quick_hint_at||lv_tick_elaps(quick_hint_at)>=3000){
        quick_hint_at=0;
        snprintf(quick_status_text,sizeof(quick_status_text),"%s",state->status?state->status:"");
    }
    quick_state.status=quick_status_text;
    if(sf_quick_is_open())quick_apply();
}
static void quick_open(void){
    if(!quick_panel)build_quick();
    quick_apply();   /* current values, also those not sent while it was closed */
    if(sf_quick_is_open()&&!lv_obj_has_flag(quick_back,LV_OBJ_FLAG_HIDDEN))return;
    lv_anim_del(quick_panel,NULL);
    quick_apply();
    lv_obj_clear_flag(quick_back,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(quick_panel,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quick_back);
    lv_obj_move_foreground(quick_panel);
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,quick_panel);lv_anim_set_exec_cb(&a,anim_y);
    lv_anim_set_values(&a,lv_obj_get_y(quick_panel),0);lv_anim_set_time(&a,320);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_timer_resume(quick_timer);
}

/* ---- Screen transitions -------------------------------------------------------
   Every transition but the book starts from a snapshot of the old screen, drawn over the
   new one and cut away by a moving edge (an LVGL draw mask): the radar sweep, a jet
   crossing, a ping ring, a sunrise, a confetti iris, a page turn, a fade. The new screen
   is live underneath from the first frame, so nothing goes dark in between.
   The book first closes its covers over the live old screen (sf_transition_ready() turns
   true once they meet), then opens on the verse.
   On the top layer, under banners and the panel; nothing on it is clickable. */
typedef enum { TS_IDLE, TS_COVERING, TS_COVERED, TS_REVEALING } tr_state_t;
static tr_state_t tr_state;
static sf_transition_t tr_kind;
static lv_obj_t *tr_obj, *tr_clip, *tr_img, *tr_flap, *tr_half[2];
static lv_img_dsc_t tr_snap;
static void *tr_snap_buf;                  /* one full-screen snapshot, PSRAM, kept for reuse */
static int32_t tr_p;                       /* progress 0-1000 */
#define TR_PAPER lv_color_hex(0xEFEADF)
#define TR_GREEN lv_color_hex(0x7CF5A3)
#define BOOK_LEATHER lv_color_hex(0x3A2316)
#define BOOK_GOLD lv_color_hex(0xD9B76A)
static void tr_finish(lv_anim_t *a){
    (void)a;
    if(tr_obj){lv_obj_del_async(tr_obj);tr_obj=NULL;}
    tr_clip=tr_img=tr_flap=tr_half[0]=tr_half[1]=NULL;
    tr_state=TS_IDLE;
}
static void tr_covered(lv_anim_t *a){(void)a;tr_state=TS_COVERED;}
static void tr_anim(void *var,lv_anim_exec_xcb_t cb,int32_t from,int32_t to,uint32_t ms,uint32_t delay,
                    lv_anim_path_cb_t path,lv_anim_ready_cb_t done){
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,var);lv_anim_set_exec_cb(&a,cb);
    lv_anim_set_values(&a,from,to);lv_anim_set_time(&a,ms);lv_anim_set_delay(&a,delay);
    lv_anim_set_path_cb(&a,path?path:lv_anim_path_linear);
    if(done)lv_anim_set_ready_cb(&a,done);
    lv_anim_start(&a);
}
static void anim_opa(void *o,int32_t v){lv_obj_set_style_opa((lv_obj_t*)o,(lv_opa_t)v,0);}
static void anim_img_fade(void *o,int32_t v){lv_obj_set_style_img_opa((lv_obj_t*)o,(lv_opa_t)v,0);}
static void tr_progress(void *o,int32_t v){tr_p=v;lv_obj_invalidate((lv_obj_t*)o);}
/* The sweep changes only the wedge from just behind its afterglow to the line: redraw that
   (its bounding box) instead of the whole screen. */
static void tr_sweep_progress(void *o,int32_t v){
    int from=(int)(360*tr_p/1000)-48, to=(int)(360*v/1000)+2;   /* afterglow is 40 degrees */
    tr_p=v;
    if(from<0)from=0;
    lv_area_t b={240,240,240,240};
    for(int d=from;;d+=5){
        if(d>to)d=to;
        float r=(float)(270+d)*3.14159265f/180.f;
        /* out to the frame's corners (340 px), not just the glass: no stale corners in the frame buffer */
        lv_coord_t x=(lv_coord_t)(240+cosf(r)*340),y=(lv_coord_t)(240+sinf(r)*340);
        if(x<b.x1)b.x1=x;
        if(x>b.x2)b.x2=x;
        if(y<b.y1)b.y1=y;
        if(y>b.y2)b.y2=y;
        if(d==to)break;
    }
    lv_area_increase(&b,4,4);
    lv_obj_invalidate_area((lv_obj_t*)o,&b);
}

/* The old screen, through a mask (NULL: all of it). */
static void tr_old(lv_draw_ctx_t *ctx,void *mask){
    int16_t id=mask?lv_draw_mask_add(mask,NULL):LV_MASK_ID_INV;
    lv_draw_img_dsc_t d;lv_draw_img_dsc_init(&d);
    lv_area_t all={0,0,479,479};
    lv_draw_img(ctx,&d,&all,&tr_snap);
    if(mask){lv_draw_mask_remove_id(id);lv_draw_mask_free_param(mask);}
}
static void tr_line(lv_draw_ctx_t *ctx,lv_color_t c,int w,lv_opa_t opa,float x1,float y1,float x2,float y2){
    lv_draw_line_dsc_t l;lv_draw_line_dsc_init(&l);
    l.color=c;l.width=(lv_coord_t)w;l.opa=opa;l.round_start=l.round_end=1;
    lv_point_t a={(lv_coord_t)lroundf(x1),(lv_coord_t)lroundf(y1)},b={(lv_coord_t)lroundf(x2),(lv_coord_t)lroundf(y2)};
    lv_draw_line(ctx,&l,&a,&b);
}
static void tr_ring(lv_draw_ctx_t *ctx,lv_color_t c,int w,lv_opa_t opa,int cx,int cy,int r){
    if(r<=0)return;
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
    d.color=c;d.width=(lv_coord_t)w;d.opa=opa;
    lv_point_t p={(lv_coord_t)cx,(lv_coord_t)cy};
    lv_draw_arc(ctx,&d,&p,(uint16_t)r,0,360);
}
/* Old screen outside a circle of radius r around (cx, cy). */
static void tr_old_outside(lv_draw_ctx_t *ctx,int cx,int cy,int r){
    if(r<=0){tr_old(ctx,NULL);return;}
    static lv_draw_mask_radius_param_t m;
    lv_area_t a={(lv_coord_t)(cx-r),(lv_coord_t)(cy-r),(lv_coord_t)(cx+r),(lv_coord_t)(cy+r)};
    lv_draw_mask_radius_init(&m,&a,LV_RADIUS_CIRCLE,true);
    tr_old(ctx,&m);
}
/* A jet seen from above, nose toward (fx, fy), centered at (x, y), k px per unit. */
static void tr_jet(lv_draw_ctx_t *ctx,float x,float y,float fx,float fy,float k,lv_opa_t opa){
    static const struct { float f1,s1,f2,s2,w; } st[]={
        {-11,0,12,0,3.2f},{2,0,-5,-13,2.4f},{2,0,-5,13,2.4f},{-9,0,-13,-5,2},{-9,0,-13,5,2},
        {0,-6,-3,-6,1.8f},{0,6,-3,6,1.8f}};   /* fuselage, wings, tailplane, engines */
    float sx=-fy,sy=fx;
    lv_color_t body=lv_color_hex(0xF5F9FF);
    for(size_t i=0;i<sizeof(st)/sizeof(st[0]);i++)
        tr_line(ctx,body,(int)lroundf(st[i].w*k),opa,x+(st[i].f1*fx+st[i].s1*sx)*k,y+(st[i].f1*fy+st[i].s1*sy)*k,
                x+(st[i].f2*fx+st[i].s2*sx)*k,y+(st[i].f2*fy+st[i].s2*sy)*k);
}
static void reveal_draw(lv_event_t *e){
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(e);
    float t=(float)tr_p/1000.f;
    switch(tr_kind){
    case SF_TRANSITION_SWEEP: {
        /* The radar's own sweep, one last turn: the next screen is already there behind it,
           under a fading green afterglow. LVGL angles: 0 = east, clockwise; north is 270. */
        int ang=(int)(360.f*t);
        if(ang<360){
            if(ang<1)tr_old(ctx,NULL);
            else{
                static lv_draw_mask_angle_param_t m;
                lv_draw_mask_angle_init(&m,240,240,(lv_coord_t)((270+ang)%360),270);
                tr_old(ctx,&m);
            }
        }
        lv_point_t c={240,240};
        lv_draw_arc_dsc_t w;lv_draw_arc_dsc_init(&w);
        w.color=TR_GREEN;w.width=240;
        for(int k=0;k<8;k++){   /* afterglow: 8 slices of 5 degrees, fading */
            int end=270+ang-k*5,start=end-5;
            if(start<270)start=270;
            if(end<=start)break;
            w.opa=(lv_opa_t)(int)((70-k*8)*(1.f-t*0.6f));
            lv_draw_arc(ctx,&w,&c,236,(uint16_t)(start%360),(uint16_t)(end%360));
        }
        float r=(float)(270+ang)*3.14159265f/180.f;
        tr_line(ctx,TR_GREEN,4,LV_OPA_COVER,240,240,240+cosf(r)*236,240+sinf(r)*236);
        break;
    }
    case SF_TRANSITION_FLYOVER: {
        /* A jet crosses from the lower left to the upper right; behind it the new page. */
        float px=-90.f+660.f*t,py=570.f-660.f*t;   /* the edge's middle, on the diagonal */
        static lv_draw_mask_line_param_t m;
        lv_draw_mask_line_points_init(&m,(lv_coord_t)(px-600),(lv_coord_t)(py-600),(lv_coord_t)(px+600),(lv_coord_t)(py+600),
                                      LV_DRAW_MASK_LINE_SIDE_TOP);
        tr_old(ctx,&m);
        const float fx=0.7071f,fy=-0.7071f,k=4.2f;
        for(int side=-1;side<=1;side+=2){   /* contrails from the engines, fading behind */
            float ex=px-fx*10*k-fy*6*k*side,ey=py-fy*10*k+fx*6*k*side;
            for(int s=0;s<5;s++){
                float a0=(float)s*60.f,a1=a0+60.f;
                lv_draw_line_dsc_t l;lv_draw_line_dsc_init(&l);   /* square ends: no bright joints */
                l.color=lv_color_hex(0xFFFFFF);l.width=(lv_coord_t)(6+s*3);l.opa=(lv_opa_t)(150-s*28);
                lv_point_t a={(lv_coord_t)lroundf(ex-fx*a0),(lv_coord_t)lroundf(ey-fy*a0)};
                lv_point_t b={(lv_coord_t)lroundf(ex-fx*a1),(lv_coord_t)lroundf(ey-fy*a1)};
                lv_draw_line(ctx,&l,&a,&b);
            }
        }
        tr_jet(ctx,px,py,fx,fy,k,LV_OPA_COVER);
        break;
    }
    case SF_TRANSITION_PING: {   /* a radar ping ring opens the scope */
        int r=(int)(345.f*t);
        tr_old_outside(ctx,240,240,r);
        tr_ring(ctx,TR_GREEN,4,LV_OPA_COVER,240,240,r);
        tr_ring(ctx,TR_GREEN,2,LV_OPA_40,240,240,r-16);
        tr_ring(ctx,TR_GREEN,1,LV_OPA_20,240,240,r-32);
        break;
    }
    case SF_TRANSITION_SUNRISE: {   /* a sun rising from below the glass */
        int r=(int)(660.f*t);
        tr_old_outside(ctx,240,600,r);
        tr_ring(ctx,lv_color_hex(0xFFC857),14,LV_OPA_40,240,600,r);
        tr_ring(ctx,lv_color_hex(0xFFE3A3),4,LV_OPA_COVER,240,600,r);
        break;
    }
    case SF_TRANSITION_CONFETTI: {   /* an iris opens from the middle, ringed with confetti */
        static const uint32_t colors[]={0xFFC857,0xFF7AA8,0x6EDCFF,0x7CF5A3,0xFFFFFF,0xC89BFF};
        int r=(int)(345.f*t);
        tr_old_outside(ctx,240,240,r);
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
        for(int i=0;i<24;i++){
            float a=(float)i*15.f*3.14159265f/180.f+t*2.f, rr=(float)r+(float)((i*7)%13)-6.f;
            int s=6+(i%3)*2;
            d.bg_color=lv_color_hex(colors[i%6]);d.radius=i%2?LV_RADIUS_CIRCLE:1;
            lv_area_t b={(lv_coord_t)(240+cosf(a)*rr-s/2),(lv_coord_t)(240+sinf(a)*rr-s/2),0,0};
            b.x2=(lv_coord_t)(b.x1+s);b.y2=(lv_coord_t)(b.y1+(i%2?s:s/2+2));
            lv_draw_rect(ctx,&d,&b);
        }
        break;
    }
    default:
        break;
    }
}

/* Page: the old screen peels away to the left, the back of the page curling over. */
static void page_set(void *o,int32_t v){
    (void)o;
    int edge=480-(int)(480*v/1000);
    int curl=(480-edge)/3;if(curl>80)curl=80;if(curl>edge)curl=edge;
    lv_obj_set_width(tr_clip,edge>0?edge:0);
    lv_obj_set_x(tr_flap,edge-curl);lv_obj_set_width(tr_flap,curl>0?curl:1);
}

/* Book: the covers close over the old screen, then open on the verse. */
static void book_set(void *o,int32_t v){   /* v: each cover's width, 0-240 */
    (void)o;
    lv_obj_set_width(tr_half[0],v);
    lv_obj_set_x(tr_half[1],480-v);lv_obj_set_width(tr_half[1],v);
    /* the cover darkens as it turns edge-on */
    lv_color_t c=lv_color_mix(BOOK_LEATHER,lv_color_hex(0x120A06),(uint8_t)(255*v/240));
    lv_obj_set_style_bg_color(tr_half[0],c,0);lv_obj_set_style_bg_color(tr_half[1],c,0);
}
static lv_obj_t *book_cover(bool left){
    lv_obj_t *h=lv_obj_create(tr_obj);plain_obj(h);
    lv_obj_set_pos(h,left?0:480,0);lv_obj_set_size(h,0,480);
    lv_obj_set_style_bg_color(h,BOOK_LEATHER,0);lv_obj_set_style_bg_opa(h,LV_OPA_COVER,0);
    /* Children stay where they are as the cover narrows (it clips them). Positions are in
       the full-width cover's frame: the right cover's spine is its left edge. */
    int off=left?0:-240;
    lv_obj_t *frame=lv_obj_create(h);plain_obj(frame);   /* gold rule inset from the edges */
    lv_obj_set_pos(frame,(left?36:12),36);lv_obj_set_size(frame,192,408);
    lv_obj_set_style_border_color(frame,BOOK_GOLD,0);lv_obj_set_style_border_width(frame,2,0);
    lv_obj_set_style_border_opa(frame,LV_OPA_70,0);
    lv_obj_set_style_border_side(frame,left?(LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM|LV_BORDER_SIDE_LEFT)
                                           :(LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM|LV_BORDER_SIDE_RIGHT),0);
    /* Gold cross on the seam, each cover carrying its half. */
    lv_obj_t *v=lv_obj_create(h);plain_obj(v);
    lv_obj_set_pos(v,240-7+off,150);lv_obj_set_size(v,14,120);
    lv_obj_set_style_bg_color(v,BOOK_GOLD,0);lv_obj_set_style_bg_opa(v,LV_OPA_COVER,0);
    lv_obj_t *hb=lv_obj_create(h);plain_obj(hb);
    lv_obj_set_pos(hb,240-38+off,182);lv_obj_set_size(hb,76,14);
    lv_obj_set_style_bg_color(hb,BOOK_GOLD,0);lv_obj_set_style_bg_opa(hb,LV_OPA_COVER,0);
    lv_obj_t *edge=lv_obj_create(h);plain_obj(edge);     /* pages showing at the opening edge */
    lv_obj_set_pos(edge,left?237:0,0);lv_obj_set_size(edge,3,480);
    lv_obj_set_style_bg_color(edge,TR_PAPER,0);lv_obj_set_style_bg_opa(edge,LV_OPA_80,0);
    return h;
}
/* The overlay itself: full screen, on the top layer, below banners and the panel. */
static lv_obj_t *tr_overlay(void){
    lv_obj_t *o=lv_obj_create(lv_layer_top());plain_obj(o);
    lv_obj_set_size(o,480,480);
    lv_obj_move_to_index(o,0);
    return o;
}
/* The old screen as it is on the glass: a copy of the panel's frame buffer (one fast copy;
   LVGL 8.2's own snapshot draws pixel by pixel, and only with an alpha channel). */
static const void *(*frame_source)(void);
void sf_screens_set_frame_source(const void *(*frame)(void)){frame_source=frame;}
static bool tr_snapshot(void){
    /* Draw what is still pending first: the pull-down panel (or a banner) has just hidden
       itself, but until that is drawn the glass still shows its last half-retracted frame. */
    lv_refr_now(NULL);
    const void *fb=frame_source?frame_source():NULL;
    if(!fb)return false;
    const uint32_t bytes=480u*480u*sizeof(lv_color_t);
    if(!tr_snap_buf)tr_snap_buf=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM);
    if(!tr_snap_buf)return false;
    memcpy(tr_snap_buf,fb,bytes);
    memset(&tr_snap,0,sizeof(tr_snap));
    tr_snap.header.cf=LV_IMG_CF_TRUE_COLOR;tr_snap.header.w=480;tr_snap.header.h=480;
    tr_snap.data_size=bytes;tr_snap.data=tr_snap_buf;
    lv_img_cache_invalidate_src(&tr_snap);
    return true;
}
bool sf_transition_begin(sf_transition_t kind){
    if(tr_state!=TS_IDLE||kind==SF_TRANSITION_NONE)return false;
    tr_kind=kind;
    if(kind!=SF_TRANSITION_BOOK&&!tr_snapshot())return false;
    tr_obj=tr_overlay();
    switch(kind){
    case SF_TRANSITION_BOOK:
        tr_half[0]=book_cover(true);tr_half[1]=book_cover(false);
        tr_state=TS_COVERING;
        tr_anim(tr_obj,book_set,0,240,420,0,lv_anim_path_ease_out,tr_covered);
        return true;
    case SF_TRANSITION_PAGE:
        tr_clip=lv_obj_create(tr_obj);plain_obj(tr_clip);
        lv_obj_set_size(tr_clip,480,480);
        tr_img=lv_img_create(tr_clip);lv_img_set_src(tr_img,&tr_snap);lv_obj_set_pos(tr_img,0,0);
        tr_flap=lv_obj_create(tr_obj);plain_obj(tr_flap);
        lv_obj_set_size(tr_flap,1,480);lv_obj_set_x(tr_flap,480);
        lv_obj_set_style_bg_opa(tr_flap,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(tr_flap,lv_color_hex(0xC9C3B6),0);    /* crease */
        lv_obj_set_style_bg_grad_color(tr_flap,TR_PAPER,0);              /* curl */
        lv_obj_set_style_bg_grad_dir(tr_flap,LV_GRAD_DIR_HOR,0);
        lv_obj_set_style_shadow_width(tr_flap,24,0);lv_obj_set_style_shadow_opa(tr_flap,LV_OPA_50,0);
        lv_obj_set_style_shadow_color(tr_flap,lv_color_hex(0x000000),0);
        break;
    case SF_TRANSITION_FADE:
        tr_img=lv_img_create(tr_obj);lv_img_set_src(tr_img,&tr_snap);lv_obj_set_pos(tr_img,0,0);
        break;
    default:   /* drawn: sweep, flyover, ping, sunrise, confetti */
        tr_p=0;
        lv_obj_add_event_cb(tr_obj,reveal_draw,LV_EVENT_DRAW_MAIN,NULL);
        break;
    }
    tr_state=TS_COVERED;
    return true;
}
bool sf_transition_ready(void){return tr_state==TS_COVERED;}
bool sf_transition_busy(void){return tr_state!=TS_IDLE;}
void sf_transition_reveal(void){
    if(tr_state!=TS_COVERED)return;
    tr_state=TS_REVEALING;
    switch(tr_kind){
    case SF_TRANSITION_SWEEP:    tr_anim(tr_obj,tr_sweep_progress,0,1000,1100,0,lv_anim_path_ease_in_out,tr_finish); break;
    case SF_TRANSITION_FLYOVER:  tr_anim(tr_obj,tr_progress,0,1000,1300,0,lv_anim_path_ease_in_out,tr_finish); break;
    case SF_TRANSITION_PING:     tr_anim(tr_obj,tr_progress,0,1000,850,0,lv_anim_path_ease_in,tr_finish); break;
    case SF_TRANSITION_SUNRISE:  tr_anim(tr_obj,tr_progress,0,1000,1000,0,lv_anim_path_ease_in_out,tr_finish); break;
    case SF_TRANSITION_CONFETTI: tr_anim(tr_obj,tr_progress,0,1000,950,0,lv_anim_path_ease_in_out,tr_finish); break;
    case SF_TRANSITION_BOOK:     tr_anim(tr_obj,book_set,240,0,850,220,lv_anim_path_ease_in_out,tr_finish); break;
    case SF_TRANSITION_PAGE:     tr_anim(tr_obj,page_set,0,1000,750,0,lv_anim_path_ease_in_out,tr_finish); break;
    default:                     tr_anim(tr_img,anim_img_fade,LV_OPA_COVER,LV_OPA_TRANSP,450,0,lv_anim_path_ease_out,tr_finish); break;
    }
}
void sf_transition_abort(void){
    if(tr_state==TS_IDLE)return;
    if(tr_obj){lv_anim_del(tr_obj,NULL);}
    if(tr_img)lv_anim_del(tr_img,NULL);
    tr_finish(NULL);
}

/* ---- Startup screen -------------------------------------------------------------
   While SkyFrame starts and joins Wi-Fi: the radar logo with its sweep turning, the
   name, the exact version, and what it is doing. Fades out when done. */
static lv_obj_t *splash_root, *splash_logo, *splash_status;
static int32_t splash_angle;
static void splash_logo_draw(lv_event_t *e){
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(e);
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
    lv_opa_t opa=lv_obj_get_style_opa(lv_obj_get_parent(lv_event_get_target(e)),0);
    lv_point_t c={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
    lv_color_t g=lv_color_hex(0x7CF5A3);
    lv_draw_arc_dsc_t ring;lv_draw_arc_dsc_init(&ring);
    ring.color=g;
    for(int i=1;i<=3;i++){ring.width=(lv_coord_t)(i==3?3:2);ring.opa=(lv_opa_t)(opa*(i==3?200:90)/255);
        lv_draw_arc(ctx,&ring,&c,(uint16_t)(i*30),0,360);}
    /* sweep: a fading wedge behind a bright line */
    lv_draw_arc_dsc_t w;lv_draw_arc_dsc_init(&w);
    w.color=g;w.width=90;
    for(int k=0;k<6;k++){
        w.opa=(lv_opa_t)(opa*(60-k*10)/255);
        int end=(int)((splash_angle+270-k*8+720)%360), start=(end-8+360)%360;
        lv_draw_arc(ctx,&w,&c,90,(uint16_t)start,(uint16_t)end);
    }
    lv_draw_line_dsc_t l;lv_draw_line_dsc_init(&l);
    l.color=g;l.width=3;l.round_end=1;l.opa=opa;
    float r=(float)(splash_angle+270)*3.14159265f/180.f;
    lv_point_t p={(lv_coord_t)lroundf(c.x+cosf(r)*90),(lv_coord_t)lroundf(c.y+sinf(r)*90)};
    lv_draw_line(ctx,&l,&c,&p);
}
static void splash_spin(void *o,int32_t v){splash_angle=v;lv_obj_invalidate((lv_obj_t*)o);}
/* The radar's sweep draws a frame at once (lv_refr_now), possibly before a deferred delete
   runs: hide the splash first, so nothing of it is drawn again, and forget every pointer. */
static void splash_gone(lv_anim_t *a){
    (void)a;
    if(!splash_root)return;
    lv_obj_add_flag(splash_root,LV_OBJ_FLAG_HIDDEN);
    lv_obj_del_async(splash_root);
    splash_root=splash_logo=splash_status=NULL;
}
void sf_splash_show(const char *version){
    if(splash_root)return;
    splash_root=lv_obj_create(lv_layer_top());plain_obj(splash_root);
    lv_obj_set_size(splash_root,480,480);
    lv_obj_set_style_bg_color(splash_root,lv_color_hex(0x0D1B33),0);
    lv_obj_set_style_bg_grad_color(splash_root,lv_color_hex(0x03060D),0);
    lv_obj_set_style_bg_grad_dir(splash_root,LV_GRAD_DIR_VER,0);
    lv_obj_set_style_bg_opa(splash_root,LV_OPA_COVER,0);
    lv_obj_add_flag(splash_root,LV_OBJ_FLAG_CLICKABLE);   /* nothing underneath reacts yet */
    splash_logo=lv_obj_create(splash_root);plain_obj(splash_logo);
    lv_obj_set_size(splash_logo,196,196);lv_obj_set_pos(splash_logo,142,64);
    lv_obj_add_event_cb(splash_logo,splash_logo_draw,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_t *plane=drawn_icon(splash_logo,ICON_PLANE,40);   /* crossing the scope */
    lv_obj_set_pos(plane,112,44);
    plain_label(splash_root,"SkyFrame",WHITE,&lv_font_montserrat_48,90,280,300);
    lv_obj_t *v=plain_label(splash_root,version?version:"",FAINT,&lv_font_montserrat_12,80,340,320);
    lv_label_set_long_mode(v,LV_LABEL_LONG_DOT);
    lv_obj_set_height(v,lv_font_get_line_height(&lv_font_montserrat_12));
    splash_status=plain_label(splash_root,"Starting...",lv_color_hex(0x7CF5A3),LV_FONT_DEFAULT,90,372,300);
    lv_anim_t a;lv_anim_init(&a);
    lv_anim_set_var(&a,splash_logo);lv_anim_set_exec_cb(&a,splash_spin);
    lv_anim_set_values(&a,0,359);lv_anim_set_time(&a,2400);
    lv_anim_set_repeat_count(&a,LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
void sf_splash_status(const char *text){if(splash_status)set_text(splash_status,text?text:"");}
bool sf_splash_visible(void){return splash_root!=NULL;}
void sf_splash_hide(void){
    if(!splash_root||lv_anim_get(splash_root,anim_opa))return;   /* gone, or already fading */
    lv_obj_clear_flag(splash_root,LV_OBJ_FLAG_CLICKABLE);
    tr_anim(splash_root,anim_opa,LV_OPA_COVER,LV_OPA_TRANSP,600,0,lv_anim_path_ease_out,splash_gone);
}
