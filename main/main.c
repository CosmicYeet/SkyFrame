/* SkyFrame v0.4 â€” Waveshare ESP32-S3-Touch-LCD-2.8C
 * Complete integration candidate: radar, live air traffic, weather,
 * photo slideshow, Wi-Fi provisioning and route lookup.
 * All LVGL calls are on app_main task. All blocking HTTPS calls in worker tasks.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/temperature_sensor.h"
#include "nvs.h"
#include <time.h>
#include "TCA9554PWR.h"
#include "I2C_Driver.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "SD_MMC.h"
#include "skyframe_engine.h"
#include "skyframe_today.h"
#include "skyframe_controller.h"
#include "skyframe_net.h"
#include "skyframe_ui.h"
#include "skyframe_screens.h"
#include "skyframe_photoplayer.h"
#include "skyframe_jpeg.h"
#include "plane_library.h"
#include "birthdays.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <strings.h>
#include <ctype.h>
#include "skyframe_sd.h"
#include "skyframe_config.h"
#include "skyframe_nvs.h"
#include "skyframe_wifi.h"
#include "skyframe_portal.h"
#include "skyframe_update.h"

#define TAG "SkyFrame"
#define AIRCRAFT_POLL_MS 20000U
#define WEATHER_POLL_MS 900000U
#define GUI_TICK_MS 5U           /* short enough for LVGL to draw on every panel refresh */
#define ROUTE_CACHE_SLOTS 24
#define WEATHER_RETRY_MS 60000U
#define TRAFFIC_STALE_MS 45000U
#define MOTION_STEP_MS 250U
#define DETAILS_TIMEOUT_MS 90000U  /* unattended: an open details panel must not stop the cycle forever */
#define PI 3.14159265358979323846

#define SETTINGS_TIMEOUT_MS 600000U   /* settings screen closes itself after 10 minutes */
#define BRIGHT_DAY 80                 /* backlight percent */
#define BRIGHT_EVENING 35             /* sunset to sunrise */
#define BRIGHT_NIGHT 8                /* night hours: soft glow, easy on the eyes */
#define BRIGHT_WAKE 50                /* touched during the night hours */
#define WAKE_MS 60000U                /* ...stays on until a minute without touches */
#define TOUR_STEP_MS 8000U             /* test mode screen tour: per screen */
#define VERSE_POLL_MS (3U*3600000U)    /* verse of the day: refresh every 3 hours */
#define VERSE_RETRY_MS 600000U
#define BIRTHDAY_SECONDS 20
#define PHOTO_MAX_BYTES (512*1024)
#define ALERTS_POLL_MS 300000U         /* weather alerts: every 5 minutes */
#define ALERTS_RETRY_MS 900000U        /* outside the US the service has none: don't keep asking */
#define SIGHTING_SECONDS 20            /* sighting banner on screen */
#define SIGHTING_REPEAT_MS (6U*3600000U)   /* the same aircraft alerts again after 6 hours */

typedef enum { MSG_TRAFFIC, MSG_WEATHER, MSG_INFO, MSG_PHOTO, MSG_VERSE, MSG_WX_ALERTS, MSG_TODAY_PHOTO } msg_kind_t;
typedef struct {
    msg_kind_t type;
    union {
        sf_aircraft_result_t aircraft;
        sf_weather_t weather;
        sf_aircraft_info_t info;
        sf_verse_t verse;
        sf_wx_alerts_t alerts;
        /* pixels NULL: no photo. Ownership travels with the message. */
        struct { char icao[9]; uint16_t *pixels; } photo;
    } data;
} app_msg_t;
typedef struct { char icao[9], callsign[12], type_code[8]; } info_req_t;
/* Keyed on airframe and callsign (the route depends on the flight); 64-bit age never wraps. */
typedef struct { sf_aircraft_info_t info; char callsign[12]; int64_t fetched_us; } info_cache_entry_t;

static sf_config_t cfg;
static sf_controller_t ctrl;
static QueueHandle_t mailbox;
static QueueHandle_t info_requests;
static sf_weather_t last_weather;
static info_cache_entry_t *info_cache;   /* ROUTE_CACHE_SLOTS entries in PSRAM */
static unsigned info_cache_next;
static uint32_t last_motion_ms;
static sf_screen_t active_screen = (sf_screen_t)-1;
static bool setup_mode;
static bool details_timing;
static uint32_t details_since;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000LL); }

static bool send_message(const app_msg_t *msg) {
    if (xQueueSend(mailbox,msg,pdMS_TO_TICKS(300)) == pdPASS) return true;
    ESP_LOGW(TAG,"UI message queue full; update skipped");
    return false;
}

static void network_task(void *arg) {
    (void)arg;
    uint32_t next_air=0, next_weather=0, next_verse=0, next_alerts=0;
    /* One message buffer for the task's lifetime (several KB, PSRAM): the queue copies it,
       so reusing it avoids allocating and freeing on every poll. */
    app_msg_t *msg=heap_caps_malloc(sizeof(*msg),MALLOC_CAP_SPIRAM);
    while (!msg) { vTaskDelay(pdMS_TO_TICKS(5000)); msg=heap_caps_malloc(sizeof(*msg),MALLOC_CAP_SPIRAM); }
    for (;;) {
        if (!sf_wifi_is_connected()) {
            /* Back online: fresh data at once. (After a very long outage the old deadlines
               would also read as far in the future on the wrapping millisecond clock.) */
            next_air=next_weather=next_verse=next_alerts=now_ms();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t now=now_ms();
        if ((int32_t)(now-next_air)>=0) {
            memset(msg,0,sizeof(*msg));
            msg->type=MSG_TRAFFIC;
            esp_err_t rc=sf_net_get_aircraft(cfg.latitude,cfg.longitude,(float)cfg.display_miles,&msg->data.aircraft);
            if (rc==ESP_OK) send_message(msg);
            else ESP_LOGW(TAG,"Aircraft request failed: %s",esp_err_to_name(rc));
            next_air=now_ms()+AIRCRAFT_POLL_MS;
        }
        now=now_ms();
        if ((int32_t)(now-next_weather)>=0) {
            memset(msg,0,sizeof(*msg));
            msg->type=MSG_WEATHER;
            esp_err_t rc=sf_net_get_weather(cfg.latitude,cfg.longitude,&msg->data.weather);
            if (rc==ESP_OK) send_message(msg);
            else ESP_LOGW(TAG,"Weather request failed: %s",esp_err_to_name(rc));
            /* Retry a failed fetch soon instead of waiting the full 15 minutes. */
            next_weather=now_ms()+(rc==ESP_OK?WEATHER_POLL_MS:WEATHER_RETRY_MS);
        }
        now=now_ms();
        if ((int32_t)(now-next_verse)>=0) {
            memset(msg,0,sizeof(*msg));
            msg->type=MSG_VERSE;
            esp_err_t rc=sf_net_get_verse(&msg->data.verse);
            if (rc==ESP_OK) send_message(msg);
            else ESP_LOGW(TAG,"Verse of the day unavailable: %s",esp_err_to_name(rc));
            next_verse=now_ms()+(rc==ESP_OK?VERSE_POLL_MS:VERSE_RETRY_MS);
        }
        now=now_ms();
        if ((int32_t)(now-next_alerts)>=0) {
            memset(msg,0,sizeof(*msg));
            msg->type=MSG_WX_ALERTS;
            esp_err_t rc=sf_net_get_wx_alerts(cfg.latitude,cfg.longitude,&msg->data.alerts);
            if (rc==ESP_OK) send_message(msg);
            else ESP_LOGW(TAG,"Weather alerts unavailable: %s",esp_err_to_name(rc));
            next_alerts=now_ms()+(rc==ESP_OK?ALERTS_POLL_MS:ALERTS_RETRY_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}

/* Download and decode the aircraft photo; NULL when there is none or it fails. */
static uint16_t *fetch_photo(const char *url) {
    uint8_t *jpeg=NULL; size_t len=0;
    if (!url[0] || sf_net_get_bytes(url,PHOTO_MAX_BYTES,&jpeg,&len)!=ESP_OK) return NULL;
    uint16_t *pixels=NULL;
    esp_err_t e=sf_jpeg_decode_fit(jpeg,len,&pixels,SKYFRAME_PHOTO_W,SKYFRAME_PHOTO_H,"aircraft photo");
    free(jpeg);
    if (e!=ESP_OK) ESP_LOGW(TAG,"Aircraft photo unreadable: %s",esp_err_to_name(e));
    return e==ESP_OK?pixels:NULL;
}

static void send_photo(app_msg_t *msg, const char *icao, uint16_t *pixels) {
    memset(msg,0,sizeof(*msg));
    msg->type=MSG_PHOTO;
    snprintf(msg->data.photo.icao,sizeof(msg->data.photo.icao),"%s",icao);
    msg->data.photo.pixels=pixels;
    if (!send_message(msg)) free(pixels);
}

/* On tap: show a photo from the card at once (this airframe if saved, else its type),
   look up the airframe + route (cached for an hour), then download the real photo and
   save it to the card for next time. */
static void info_task(void *arg) {
    (void)arg;
    info_req_t req;
    app_msg_t *msg=heap_caps_calloc(1,sizeof(*msg),MALLOC_CAP_SPIRAM);
    if (!msg) { ESP_LOGE(TAG,"No memory for aircraft lookups"); vTaskDelete(NULL); }
    while (xQueueReceive(info_requests,&req,portMAX_DELAY)==pdPASS) {
        bool have_exact=false, have_any=false;
        uint16_t *px=plane_library_load_saved(req.icao);
        if (px) { send_photo(msg,req.icao,px); have_exact=have_any=true; }
        else if ((px=plane_library_load_type(req.type_code))) { send_photo(msg,req.icao,px); have_any=true; }

        memset(msg,0,sizeof(*msg));
        msg->type=MSG_INFO;
        sf_aircraft_info_t *info=&msg->data.info;
        bool cached=false;
        for (int i=0;i<ROUTE_CACHE_SLOTS && info_cache;i++) {
            info_cache_entry_t *e=&info_cache[i];
            if (e->info.icao[0] && !strcasecmp(e->info.icao,req.icao) && !strcmp(e->callsign,req.callsign) &&
                esp_timer_get_time()-e->fetched_us<3600000000LL) { *info=e->info; cached=true; break; }
        }
        if (!cached) {
            esp_err_t rc=sf_wifi_is_connected()?sf_net_get_aircraft_info(req.icao,req.callsign,info):ESP_ERR_INVALID_STATE;
            if (rc!=ESP_OK) {
                memset(info,0,sizeof(*info));
                snprintf(info->icao,sizeof(info->icao),"%s",req.icao);
                /* Non-ICAO addresses (adsb.fi prefixes them with '~') have no database record:
                   that is "not available", not a failed lookup. */
                info->lookup_ok=(rc==ESP_ERR_INVALID_ARG);
                if (!info->lookup_ok) ESP_LOGW(TAG,"Aircraft lookup failed: %s (%s)",req.icao,esp_err_to_name(rc));
            } else if (info_cache) {
                info_cache_entry_t *e=&info_cache[info_cache_next++%ROUTE_CACHE_SLOTS];
                e->info=*info; e->fetched_us=esp_timer_get_time();
                snprintf(e->callsign,sizeof(e->callsign),"%s",req.callsign);
            }
        }
        char url[sizeof(info->photo_url)];
        snprintf(url,sizeof(url),"%s",info->photo_url);
        send_message(msg);

        /* The saved photo is already the real one; skip the download, and skip it
           entirely if another plane was tapped meanwhile. */
        if (have_exact || uxQueueMessagesWaiting(info_requests)) continue;
        px=fetch_photo(url);
        if (px) {
            plane_library_save(req.icao,px);
            send_photo(msg,req.icao,px);
        } else if (!have_any) send_photo(msg,req.icao,NULL);
    }
    vTaskDelete(NULL);
}

static void selected_plane(const skyframe_aircraft_t *a) {
    if (!a || !a->icao[0] || !info_requests) return;
    info_req_t req={0};
    snprintf(req.icao,sizeof(req.icao),"%s",a->icao);
    /* The radar falls back to the ICAO address or "UNKNOWN" when no callsign is broadcast;
       those are not callsigns, so don't ask for a route with them. */
    if (strcasecmp(a->callsign,a->icao) && strcmp(a->callsign,"UNKNOWN"))
        snprintf(req.callsign,sizeof(req.callsign),"%s",a->callsign);
    snprintf(req.type_code,sizeof(req.type_code),"%s",a->type_code);
    /* Keep only most recent selection if tapped repeatedly. */
    xQueueOverwrite(info_requests,&req);
}

/* Long press: on to the next screen of the rotation, which then runs its normal time. */
static void cycle_screen(void) {
    sf_schedule_skip(&ctrl.scheduler,now_ms());
}

static void count_today(const sf_aircraft_result_t *result);
static void check_sightings(const sf_aircraft_result_t *result);
static void apply_aircraft(const sf_aircraft_result_t *result) {
    sf_controller_aircraft(&ctrl,result,now_ms());
    if(result->api_ok) { count_today(result); check_sightings(result); }
    ESP_LOGI(TAG,"Aircraft: %u within %u miles, ignored %u",(unsigned)result->count,
             cfg.display_miles,(unsigned)result->ignored);
}

static void show_weather(const sf_weather_t *w) {
    last_weather=*w;
    sf_weather_update(w);
}

/* ---- Test mode state (menu opened by holding the settings screen for 5 s) ---- */
#define TEST_NONE SF_TEST_COUNT
#define TEST_MENU_IDLE_MS 600000U     /* menu closes itself after 10 minutes */
static bool test_mode;
static sf_test_action_t test_running=TEST_NONE;
static uint32_t test_started, test_menu_since;
static bool test_is(sf_test_action_t a) { return test_mode && test_running==a; }
static int bday_preview_index;            /* family birthday test: which upcoming one is shown */
static void end_test(void);
static bool settings_open;                /* defined with the settings screen below */
static bool setup_up;                     /* setup screen shown (screens underneath paused) */
/* Severe-weather alerts, Today's sky and sighting alerts: defined further down. */
static bool wx_alerts_changed;            /* new alerts, or a test began or ended: redraw the card */
static sf_today_t *today;                 /* today's tally (PSRAM) */
static void refresh_today(void);
static void request_today_photo(const char *type);
static void show_sample_today(void);
static void show_sample_sighting(void);
static bool sample_banner;              /* the sighting banner up is a test sample */
static bool dismissed_in_test;          /* a real birthday was "dismissed" during a test (not saved) */
static void show_sample_overhead(void);
static void start_splash(void);

/* Four sample planes circling the radar, one in each special color. */
static int demo_traffic(skyframe_aircraft_t *out, uint32_t t) {
    static const struct { const char *icao,*cs,*type,*desc,*squawk,*emergency,*cat; unsigned flags; int alt,rate; float kt; } d[]={
        {"~DEMO1","UAL77","B738","BOEING 737-800","7700","general","A3",0,6200,-1800,240},
        {"~DEMO2","AF1","VC25","BOEING VC-25A","2345","none","A5",1,21000,0,450},
        {"~DEMO3","RCH451","C17","BOEING C-17 GLOBEMASTER III","4521","none","A5",1,14000,1200,330},
        {"~DEMO4","LIFE21","EC35","EUROCOPTER EC-135","1200","lifeguard","A7",0,1500,0,125},
    };
    int n=(int)(sizeof(d)/sizeof(d[0]));
    for(int i=0;i<n;i++) {
        skyframe_aircraft_t *a=&out[i];
        memset(a,0,sizeof(*a));
        snprintf(a->icao,sizeof(a->icao),"%s",d[i].icao);
        snprintf(a->callsign,sizeof(a->callsign),"%s",d[i].cs);
        snprintf(a->type_code,sizeof(a->type_code),"%s",d[i].type);
        snprintf(a->aircraft_type,sizeof(a->aircraft_type),"%s",d[i].desc);
        snprintf(a->squawk,sizeof(a->squawk),"%s",d[i].squawk);
        snprintf(a->emergency,sizeof(a->emergency),"%s",d[i].emergency);
        snprintf(a->category,sizeof(a->category),"%s",d[i].cat);
        a->db_flags=d[i].flags;
        a->altitude_ft=d[i].alt;a->vert_rate_fpm=d[i].rate;a->has_vert_rate=true;a->speed_knots=d[i].kt;
        a->bearing_deg=fmodf(i*90.f+(float)(t%600000u)*0.0006f,360.f);   /* slow clockwise circle */
        a->distance_miles=(float)cfg.display_miles*(0.35f+0.12f*(float)i);
        a->heading_deg=fmodf(a->bearing_deg+90.f,360.f);
    }
    return n;
}

static void project_traffic(uint32_t time) {
    if (test_is(SF_TEST_TRAFFIC)) {
        skyframe_aircraft_t demo[4];
        int n=demo_traffic(demo,time);
        skyframe_ui_set_status("Sample traffic (test mode)");
        skyframe_ui_set_aircraft(demo,(uint8_t)n);
        skyframe_ui_set_airport_ground(0);
        return;
    }
    /* Dead reckoning between network snapshots makes planes glide in correct
       direction instead of jumping to new coordinates every 20 seconds. */
    /* No data or stale data: clear the scope and say so, never freeze old positions. */
    const char *status=NULL;
    if (!ctrl.have_aircraft_snapshot || !ctrl.aircraft.api_ok)
        status=sf_wifi_is_connected()?"Waiting for traffic data":"No Wi-Fi  \xE2\x80\xA2  traffic unavailable";
    else if ((uint32_t)(time-ctrl.aircraft_updated_ms)>TRAFFIC_STALE_MS)
        status=sf_wifi_is_connected()?"Traffic data unavailable":"No Wi-Fi  \xE2\x80\xA2  traffic unavailable";
    skyframe_ui_set_status(status);
    if (status) { skyframe_ui_set_aircraft(NULL,0); skyframe_ui_set_airport_ground(0); return; }
    skyframe_ui_set_airport_ground((unsigned)ctrl.aircraft.airport_ground);
    skyframe_aircraft_t traffic[SF_MAX_PLANES]={0};
    uint32_t age=time-ctrl.aircraft_updated_ms;
    size_t n=ctrl.aircraft.count;
    for(size_t i=0;i<n;i++) {
        const sf_plane_t *a=&ctrl.aircraft.planes[i];
        skyframe_aircraft_t *t=&traffic[i];
        snprintf(t->callsign,sizeof(t->callsign),"%s",a->callsign);
        snprintf(t->aircraft_type,sizeof(t->aircraft_type),"%s",a->aircraft_type);
        snprintf(t->icao,sizeof(t->icao),"%s",a->icao);
        snprintf(t->registration,sizeof(t->registration),"%s",a->registration);
        snprintf(t->type_code,sizeof(t->type_code),"%s",a->type_code);
        snprintf(t->operator_name,sizeof(t->operator_name),"%s",a->operator_name);
        snprintf(t->year,sizeof(t->year),"%s",a->year);
        snprintf(t->squawk,sizeof(t->squawk),"%s",a->squawk);
        snprintf(t->category,sizeof(t->category),"%s",a->category);
        snprintf(t->emergency,sizeof(t->emergency),"%s",a->emergency);
        t->db_flags=a->db_flags;
        t->vert_rate_fpm=a->vert_rate_fpm;t->has_vert_rate=a->has_vert_rate;
        t->altitude_ft=a->altitude_ft;
        t->speed_knots=a->speed_knots;t->heading_deg=a->heading_deg;
        t->on_ground=a->on_ground;
        float angle=a->bearing_deg*(float)(PI/180.0);
        float east=sinf(angle)*a->distance_miles;
        float north=cosf(angle)*a->distance_miles;
        if(a->has_speed && a->has_heading && !a->on_ground) {
            /* Project from when the position was actually reported, not when we received it. */
            float hours=((float)age+a->position_age_s*1000.f)/3600000.f;
            float miles=a->speed_knots*1.15077945f*hours;
            float track=a->heading_deg*(float)(PI/180.0);
            east+=sinf(track)*miles;north+=cosf(track)*miles;
        }
        t->distance_miles=hypotf(east,north);
        t->bearing_deg=fmodf(atan2f(east,north)*180.f/(float)PI+360.f,360.f);
    }
    skyframe_ui_set_aircraft(traffic,(uint8_t)n);
}

/* ---- Verse of the day and birthdays (optional screens in the rotation) ---- */
static bool verse_ready;
static birthday_list_t birthdays;   /* today's birthdays not yet dismissed; main owns the photos */
static int birthday_next, birthday_shown, today_year;
static void update_optional_screens(void) {
    sf_schedule_set_optional(&ctrl.scheduler,verse_ready?cfg.verse_seconds:0,birthdays.count?BIRTHDAY_SECONDS:0);
}
static void show_birthday(int index) {
    sf_birthday_view_t view={.name="",.age=-1};
    if(birthdays.count) {
        birthday_shown=index%birthdays.count;
        const birthday_today_t *b=&birthdays.list[birthday_shown];
        view=(sf_birthday_view_t){.name=b->who.name,.age=b->age,.month=b->who.month,.day=b->who.day,
            .owner=b->who.owner,.message=birthdays.message[0]?birthdays.message:DEFAULT_OWNER_MESSAGE,.pixels=b->pixels,.photo_size=BIRTHDAY_PHOTO_SIZE};
    }
    sf_birthday_set(&view);   /* also releases the screen's hold on any previous photo */
}
/* A new list from the loader (new day, or the card changed). */
static void take_birthdays(void) {
    birthday_list_t fresh;
    if(!birthdays_take(&fresh)) return;
    /* During a birthday test only its preview is shown; afterwards previews are stale. */
    bool testing=test_is(SF_TEST_OWNER_BIRTHDAY)||test_is(SF_TEST_FAMILY_BIRTHDAY);
    if(fresh.preview!=testing) {
        for(int i=0;i<fresh.count;i++) free(fresh.list[i].pixels);
        return;
    }
    birthday_list_t old=birthdays;
    birthdays=fresh;
    birthday_next=0;
    show_birthday(0);
    for(int i=0;i<old.count;i++) free(old.list[i].pixels);
    update_optional_screens();   /* nobody left: the rotation leaves the birthday page at once */
}
/* "Dismiss until next year" / "Thank you!" */
static void dismiss_birthday(void) {
    if(!birthdays.count) return;
    int i=birthday_shown;
    /* Tests never save, even if a real birthday is on screen while one runs. */
    if(!birthdays.preview && !test_mode) birthdays_dismiss(&birthdays.list[i].who,today_year);
    else if(!birthdays.preview) dismissed_in_test=true;
    uint16_t *px=birthdays.list[i].pixels;
    memmove(&birthdays.list[i],&birthdays.list[i+1],sizeof(birthdays.list[0])*(size_t)(birthdays.count-i-1));
    birthdays.count--;
    show_birthday(i);         /* next person, or an empty page that the rotation leaves */
    free(px);
    update_optional_screens();
    if(birthdays.preview && !birthdays.count) end_test();
}

/* ---- Test mode actions ---- */
static void clear_preview_birthdays(void) {
    if(!birthdays.preview) return;
    birthday_list_t old=birthdays;
    memset(&birthdays,0,sizeof(birthdays));
    show_birthday(0);                 /* screen lets go of the preview photos first */
    for(int i=0;i<old.count;i++) free(old.list[i].pixels);
    update_optional_screens();
    birthdays_refresh();              /* bring back today's real reminders, if any */
}
static void end_test(void) {         /* back to the test menu */
    /* Over first: the restores below (refresh_today...) skip their work while it runs. */
    sf_test_action_t was=test_running;
    test_running=TEST_NONE;
    bool birthday_test=was==SF_TEST_OWNER_BIRTHDAY||was==SF_TEST_FAMILY_BIRTHDAY;
    /* Put back what the previews replaced. */
    if(sample_banner) { sample_banner=false; sf_sighting_show(NULL); }
    if(was==SF_TEST_SPLASH) sf_splash_hide();
    if(was==SF_TEST_TODAY) refresh_today();
    if(was==SF_TEST_WX_ALERT) { wx_alerts_changed=true; sf_weather_close_alert(); }
    clear_preview_birthdays();
    /* Real lists that arrived during a birthday test were set aside, and a real birthday
       "dismissed" during any test was only hidden: reload today's. */
    if(birthday_test || dismissed_in_test) { dismissed_in_test=false; birthdays_refresh(); }
    test_menu_since=now_ms();
    sf_test_badge(NULL);
    sf_test_next(false);
}
static void enter_test_mode(void) {
    settings_open=false;
    test_mode=true;
    end_test();
}
static void test_action(sf_test_action_t a) {
    switch(a) {
    case SF_TEST_EXIT: end_test(); test_mode=false; sf_test_menu_show(false); return;
    case SF_TEST_STOP: if(test_mode && test_running!=TEST_NONE) end_test(); return;
    case SF_TEST_NEXT:   /* the family birthday after the one shown, in date order */
        if(test_is(SF_TEST_FAMILY_BIRTHDAY)) { birthdays_preview_family(++bday_preview_index); test_started=now_ms(); }
        return;
    case SF_TEST_RESTART: sf_planned_restart(); return;
    case SF_TEST_UPDATE: sf_update_check_now(true); return;   /* progress shows under System */
    case SF_TEST_OWNER_BIRTHDAY: birthdays_preview(true); break;
    case SF_TEST_FAMILY_BIRTHDAY: bday_preview_index=0; birthdays_preview_family(0); break;
    default: break;
    }
    test_running=a;
    test_started=now_ms();
    if(a==SF_TEST_SIGHTING) show_sample_sighting();
    if(a==SF_TEST_OVERHEAD) show_sample_overhead();
    if(a==SF_TEST_SPLASH) start_splash();
    if(a==SF_TEST_TODAY) show_sample_today();
    if(a==SF_TEST_WX_ALERT) wx_alerts_changed=true;
}
static uint32_t test_length_ms(sf_test_action_t a) {
    switch(a) {
    case SF_TEST_OWNER_BIRTHDAY: case SF_TEST_FAMILY_BIRTHDAY: return 30000;
    case SF_TEST_TRAFFIC: return 60000;
    case SF_TEST_NIGHT: return 90000;
    case SF_TEST_TOUR: return 5*TOUR_STEP_MS;
    case SF_TEST_USB: return 10000;
    case SF_TEST_WIFI: return 15000;
    case SF_TEST_WX_ALERT: return 45000;
    case SF_TEST_SIGHTING: return 25000;
    case SF_TEST_TODAY: return 20000;
    case SF_TEST_OVERHEAD: return 12000;
    case SF_TEST_SPLASH: return 7000;
    default: return 0;
    }
}
/* CPU busy per core since the previous sample, from the idle tasks' run time (same clock:
   esp_timer microseconds, 32-bit, so samples must be under ~71 minutes apart). cpu[] is -1
   on the first sample or after more than max_us. */
typedef struct { uint32_t idle[2], at; } cpu_sampler_t;
static void cpu_sample(cpu_sampler_t *s, uint32_t max_us, int cpu[2]) {
    uint32_t now=(uint32_t)esp_timer_get_time(), span=now-s->at;
    for(int core=0;core<2;core++) {
        TaskStatus_t st;
        vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core),&st,pdFALSE,eInvalid);
        uint32_t idle=(uint32_t)st.ulRunTimeCounter;
        cpu[core]=-1;
        if(s->at && span && span<=max_us) {
            int busy=100-(int)((uint64_t)(uint32_t)(idle-s->idle[core])*100u/span);
            cpu[core]=busy<0?0:busy>100?100:busy;
        }
        s->idle[core]=idle;
    }
    s->at=now?now:1;
}
/* ---- Hardware readings for the test menu and the log ---- */
/* Battery sense, per Waveshare's ESP32-S3-Touch-LCD-2.8C wiki (BAT_ADC = GPIO4) and its
   demo BAT_Driver: ADC1 channel 3 behind a 1:3 divider, scaled by their 0.980952 correction.
   Without a battery the pin shows the charger's output instead. */
#define BAT_ADC_CHANNEL ADC_CHANNEL_3
#define BAT_DIVIDER 3.0f
#define BAT_CORRECTION 0.980952f
static adc_oneshot_unit_handle_t bat_adc;
static adc_cali_handle_t bat_cali;
static temperature_sensor_handle_t chip_sensor;
static uint32_t boot_count;
static void hardware_stats_init(void) {
    nvs_handle_t h;   /* boot count: one small NVS write per start */
    if(nvs_open("skyframe",NVS_READWRITE,&h)==ESP_OK) {
        uint32_t n=0;
        nvs_get_u32(h,"boot_count",&n);
        boot_count=n+1;
        if(nvs_set_u32(h,"boot_count",boot_count)==ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    temperature_sensor_config_t tc=TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10,80);
    if(temperature_sensor_install(&tc,&chip_sensor)!=ESP_OK || temperature_sensor_enable(chip_sensor)!=ESP_OK) {
        ESP_LOGW(TAG,"Chip temperature sensor unavailable");
        chip_sensor=NULL;
    }
    adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1};
    adc_oneshot_chan_cfg_t chan={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    adc_cali_curve_fitting_config_t cal={.unit_id=ADC_UNIT_1,.chan=BAT_ADC_CHANNEL,.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    if(adc_oneshot_new_unit(&unit,&bat_adc)!=ESP_OK) bat_adc=NULL;
    else if(adc_oneshot_config_channel(bat_adc,BAT_ADC_CHANNEL,&chan)!=ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&cal,&bat_cali)!=ESP_OK) bat_cali=NULL;   /* uncalibrated: no reading */
}
static float chip_celsius(void) {
    float c;
    return chip_sensor && temperature_sensor_get_celsius(chip_sensor,&c)==ESP_OK ? c : NAN;
}
static float battery_volts(void) {
    if(!bat_adc||!bat_cali) return NAN;
    int sum=0, raw, mv;
    for(int i=0;i<8;i++) {   /* a few samples smooth out ADC noise; microseconds each */
        if(adc_oneshot_read(bat_adc,BAT_ADC_CHANNEL,&raw)!=ESP_OK) return NAN;
        sum+=raw;
    }
    if(adc_cali_raw_to_voltage(bat_cali,sum/8,&mv)!=ESP_OK) return NAN;
    return (float)mv*BAT_DIVIDER/1000.f/BAT_CORRECTION;
}
static const char *reset_reason_text(void) {
    if(sf_restart_was_planned()) return "Restarted by SkyFrame";   /* even if a watchdog finished it */
    switch(esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power switched on";
    case ESP_RST_SW: return "Restarted by SkyFrame";
    case ESP_RST_PANIC: return "Software crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "Froze, auto-restart";
    case ESP_RST_BROWNOUT: return "Power dipped too low";
    case ESP_RST_PWR_GLITCH: return "Power glitch";
    case ESP_RST_EXT: return "Reset button";
    case ESP_RST_USB: case ESP_RST_JTAG: return "Reset over USB";
    case ESP_RST_DEEPSLEEP: return "Woke from sleep";
    default: return "Unknown";
    }
}
/* microSD free/total in GB (NAN: no card or on a computer). Asking the card holds its busy
   count and may read the FAT, so at most every 30 s. */
static void sd_space(uint32_t t, float *free_gb, float *total_gb) {
    static uint32_t at;
    static float f=NAN, total=NAN;
    if(!at || (uint32_t)(t-at)>=30000) {
        at=t?t:1; f=total=NAN;
        uint64_t tb=0, fb=0;
        if(sf_sd_begin()) {
            if(esp_vfs_fat_info(SF_SD_ROOT,&tb,&fb)==ESP_OK && tb) { total=(float)tb/1e9f; f=(float)fb/1e9f; }
            sf_sd_end();
        }
    }
    *free_gb=f; *total_gb=total;
}
/* Every 30 s to the serial log, whatever is on screen (the test menu pauses the screens
   underneath, so its own numbers don't show what the radar costs). */
static void log_resources(uint32_t t) {
    static uint32_t last;
    static cpu_sampler_t s;
    if(!last) {   /* first call is at boot, from the main loop */
        hardware_stats_init();
        ESP_LOGI(TAG,"Start #%u, last reset: %s",(unsigned)boot_count,reset_reason_text());
    }
    if(last && (uint32_t)(t-last)<30000) return;
    last=t?t:1;
    int cpu[2];
    cpu_sample(&s,40000000u,cpu);
    if(cpu[0]<0) return;
    static const char *const names[SF_SCREEN_COUNT]={"radar","weather","photos","verse","birthday","today"};
    ESP_LOGI(TAG,"CPU %d%% / %d%%  RAM %u KB free (lowest %u, block %u)  PSRAM %u KB free  chip %.0fC  [%s]",cpu[0],cpu[1],
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)/1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)/1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024),chip_celsius(),
             (unsigned)active_screen<SF_SCREEN_COUNT?names[active_screen]:"?");
}
/* "SkyFrame v0.5.0  *  Nov 23 2026" and "Nov 23 2026 14:02:11". The version comes from
   version.txt (the app description); __DATE__ pads single-digit days with a space, squeezed out
   here. */
static void squeeze_spaces(char *s) {
    char *o=s;
    for(const char *p=s;*p;p++) if(!(*p==' ' && o>s && o[-1]==' ')) *o++=*p;
    *o=0;
}
static void build_name(char *version, size_t vcap, char *built, size_t bcap) {
    const esp_app_desc_t *d=esp_app_get_description();
    char date[16];
    snprintf(date,sizeof(date),"%s",d->date);
    squeeze_spaces(date);
    snprintf(version,vcap,"SkyFrame v%.16s  \xE2\x80\xA2  %.11s",d->version,date);
    snprintf(built,bcap,"%s %.8s",date,d->time);
}
/* Everything the test menu's System panel shows. Called once a second. */
#define UPDATE_HOUR 3                   /* nightly update check: 3-4 AM local time */
static char crash_text[48];             /* the last saved crash (read once at boot) */
static void system_stats(uint32_t t, sf_system_stats_t *s) {
    static cpu_sampler_t cs;
    memset(s,0,sizeof(*s));
    cpu_sample(&cs,3000000u,s->cpu);   /* menu just reopened: a fresh sample first */
    s->ram_kb=(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024);
    s->ram_low_kb=(unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)/1024);
    s->ram_block_kb=(unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)/1024);
    s->psram_free_mb=(float)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1048576.f;
    s->psram_total_mb=(float)heap_caps_get_total_size(MALLOC_CAP_SPIRAM)/1048576.f;
    sd_space(t,&s->sd_free_gb,&s->sd_total_gb);
    s->chip_c=chip_celsius();
    s->battery_v=battery_volts();
    wifi_ap_record_t ap;
    if(sf_wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap)==ESP_OK) { s->rssi=ap.rssi; s->channel=ap.primary; }
    if(!sf_wifi_ip(s->ip,sizeof(s->ip))) s->ip[0]=0;
    s->cpu_mhz=(unsigned)esp_rom_get_cpu_ticks_per_us();
    s->uptime_s=(uint32_t)(esp_timer_get_time()/1000000);
    s->boots=boot_count;
    s->reset_reason=reset_reason_text();
    build_name(s->version,sizeof(s->version),s->built,sizeof(s->built));
    sf_update_status_t us;
    sf_update_status(&us);
    if(sf_update_pending_verify()) snprintf(s->update,sizeof(s->update),"v%s: new, settling in",sf_update_version());
    else if(us.text[0]) snprintf(s->update,sizeof(s->update),"%s",us.text);
    else snprintf(s->update,sizeof(s->update),"Checked nightly at %d AM",UPDATE_HOUR);
    snprintf(s->crash,sizeof(s->crash),"%s",crash_text);
}

/* Runs the menu / current test. True when the test decides which screen shows. */
static bool test_tick(uint32_t t, sf_screen_t *forced) {
    if(!test_mode) { sf_test_menu_show(false); return false; }
    if(test_running==TEST_NONE) {
        /* System panel and web page addresses, refreshed once a second (the IP can change). */
        static uint32_t info_at;
        if(!info_at||(uint32_t)(t-info_at)>=1000) {
            info_at=t?t:1;
            sf_system_stats_t stats;
            system_stats(t,&stats);
            sf_test_menu_set_stats(&stats);
        }
        sf_test_menu_show(true);
        if((uint32_t)(t-test_menu_since)>=TEST_MENU_IDLE_MS && lv_disp_get_inactive_time(NULL)>=TEST_MENU_IDLE_MS) {
            test_mode=false; sf_test_menu_show(false);
        }
        return false;
    }
    sf_test_menu_show(false);
    uint32_t elapsed=t-test_started;
    if(elapsed>=test_length_ms(test_running)) { end_test(); return false; }
    switch(test_running) {
    case SF_TEST_OWNER_BIRTHDAY: case SF_TEST_FAMILY_BIRTHDAY:
        /* Only once the preview has loaded, so a real birthday is never shown as the test. */
        if(!birthdays.preview) { sf_test_badge("TEST  \xE2\x80\xA2  loading preview"); return false; }
        sf_test_badge("TEST  \xE2\x80\xA2  birthday preview");
        sf_test_next(test_running==SF_TEST_FAMILY_BIRTHDAY);
        *forced=SF_SCREEN_BIRTHDAY; return true;
    case SF_TEST_TRAFFIC:
        sf_test_badge("TEST  \xE2\x80\xA2  tap a plane");
        *forced=SF_SCREEN_RADAR; return true;
    case SF_TEST_TOUR: {
        sf_screen_t tour[6]; int n=0;
        tour[n++]=SF_SCREEN_RADAR; tour[n++]=SF_SCREEN_WEATHER;
        if(today && today->planes) tour[n++]=SF_SCREEN_TODAY;
        if(verse_ready) tour[n++]=SF_SCREEN_VERSE;
        tour[n++]=SF_SCREEN_PHOTOS;
        if(birthdays.count) tour[n++]=SF_SCREEN_BIRTHDAY;
        int i=(int)(elapsed/TOUR_STEP_MS);
        if(i>=n) { end_test(); return false; }
        sf_test_badge("TEST  \xE2\x80\xA2  screen tour");
        *forced=tour[i]; return true;
    }
    case SF_TEST_NIGHT: sf_test_badge("TEST  \xE2\x80\xA2  tap to brighten"); return false;
    case SF_TEST_USB:   sf_test_badge("TEST  \xE2\x80\xA2  USB screen"); return false;
    case SF_TEST_WIFI:  sf_test_badge("TEST  \xE2\x80\xA2  Wi-Fi screen"); return false;
    case SF_TEST_WX_ALERT:
        sf_test_badge("TEST  \xE2\x80\xA2  tap the alert");
        *forced=SF_SCREEN_WEATHER; return true;
    case SF_TEST_SIGHTING:   /* over another screen, as it would be */
        sf_test_badge("TEST  \xE2\x80\xA2  sighting alert");
        *forced=SF_SCREEN_WEATHER; return true;
    case SF_TEST_TODAY:
        sf_test_badge("TEST  \xE2\x80\xA2  sample day");
        *forced=SF_SCREEN_TODAY; return true;
    case SF_TEST_OVERHEAD:
        sf_test_badge("TEST  \xE2\x80\xA2  overhead now");
        *forced=SF_SCREEN_WEATHER; return true;
    case SF_TEST_SPLASH:   /* as at power-on: joining, then connected */
        sf_splash_status(elapsed<3500?"Joining Wi-Fi...":"Connected");
        return false;
    default: return false;
    }
}

/* covered: nothing of the screen can be seen (a full-screen overlay such as the test menu,
   setup or USB notice is up, or the backlight is off). It is hidden then, so the sweep,
   slideshow and redraws stop instead of rendering behind the overlay. */
static bool covered_now;                  /* the shown screen is hidden under a notice */
static void display_screen(sf_screen_t screen, bool covered) {
    if(active_screen==screen && covered_now==covered)return;
    if(active_screen!=screen && screen==SF_SCREEN_BIRTHDAY) show_birthday(birthday_next++);   /* several birthdays take turns */
    active_screen=screen;
    covered_now=covered;
    if(covered) screen=(sf_screen_t)-1;   /* matches none: all hidden */
    sf_verse_show(screen==SF_SCREEN_VERSE);
    sf_birthday_show(screen==SF_SCREEN_BIRTHDAY);
    skyframe_ui_show(screen==SF_SCREEN_RADAR);
    sf_weather_show(screen==SF_SCREEN_WEATHER);
    sf_photo_show(screen==SF_SCREEN_PHOTOS);
    sf_today_show(screen==SF_SCREEN_TODAY);
}

/* Rotation changes animate, each in its page's theme: the radar's last sweep wipes it away,
   a jet crosses into Today's sky, the verse opens like a book, photos turn in like a page,
   birthdays open with confetti, the radar returns with a ping, the weather with a sunrise. A notice coming up (or going
   away) switches at once. */
static sf_transition_t transition_for(sf_screen_t from, sf_screen_t to) {
    if(from==SF_SCREEN_RADAR) return SF_TRANSITION_SWEEP;   /* the radar's last sweep */
    switch(to) {
    case SF_SCREEN_TODAY:    return SF_TRANSITION_FLYOVER;
    case SF_SCREEN_VERSE:    return SF_TRANSITION_BOOK;
    case SF_SCREEN_PHOTOS:   return SF_TRANSITION_PAGE;
    case SF_SCREEN_BIRTHDAY: return SF_TRANSITION_CONFETTI;
    case SF_SCREEN_RADAR:    return SF_TRANSITION_PING;
    case SF_SCREEN_WEATHER:  return SF_TRANSITION_SUNRISE;
    default:                 return SF_TRANSITION_FADE;
    }
}
static void show_screen(sf_screen_t target, bool covered) {
    if(covered || covered_now || (unsigned)active_screen>=SF_SCREEN_COUNT || sf_splash_visible() ||
       (cfg.ui_flags&SF_UI_NO_TRANSITIONS)) {   /* turned off in the settings: switch at once */
        sf_transition_abort();
        display_screen(target,covered);
        return;
    }
    if(sf_transition_ready()) { display_screen(target,false); sf_transition_reveal(); return; }
    if(sf_transition_busy() || target==active_screen) return;   /* mid-transition: the next waits */
    /* The panel slides away and a banner leaves first, so neither is frozen into the picture
       of the old screen that the transition wipes away. */
    if(sf_quick_is_open() || sf_sighting_visible()) return;
    if(!sf_transition_begin(transition_for(active_screen,target))) { display_screen(target,false); return; }
    if(sf_transition_ready()) { display_screen(target,false); sf_transition_reveal(); }
}

/* ---- Setup / settings screen and the setup network ------------------------------
   Shown on first boot, when the saved Wi-Fi looks wrong (never connected since boot),
   or on request (5 s hold). The "SkyFrame-XXXX" setup network is only on while that
   screen is up; otherwise the settings page is reached at http://skyframe.local. */
static bool settings_open;
static uint32_t settings_since;
static void open_settings(void) { settings_open=true; settings_since=now_ms(); }
static void close_settings(void) { settings_open=false; }
static void setup_banner(uint32_t t) {
    if(settings_open && (uint32_t)(t-settings_since)>=SETTINGS_TIMEOUT_MS) settings_open=false;
    enum { HIDDEN, FIRST_SETUP, CANT_JOIN, SETTINGS } want =
        setup_mode?FIRST_SETUP:(sf_wifi_needs_setup()||test_is(SF_TEST_WIFI))?CANT_JOIN:settings_open?SETTINGS:HIDDEN;
    static char ip[16];
    static uint32_t ip_checked;
    if(!ip_checked||(uint32_t)(t-ip_checked)>=1000){ ip_checked=t?t:1; ip[0]=0; sf_wifi_ip(ip,sizeof(ip)); }
    static int shown=-1;
    static char shown_ip[16];
    if((int)want==shown && !strcmp(ip,shown_ip)) return;
    shown=(int)want;
    snprintf(shown_ip,sizeof(shown_ip),"%s",ip);
    /* Switching Wi-Fi mode mid-connection wedges the driver's connection attempt, so the
       setup network only goes off once connected (ip known); it re-evaluates when the
       address appears or disappears. */
    static bool joined;               /* connected at least once since boot */
    if(ip[0]) joined=true;
    sf_portal_set_ap(want!=HIDDEN || !joined);
    setup_up=want!=HIDDEN;
    if(want==HIDDEN){sf_setup_show(false,NULL);return;}
    char ssid[32],pwd[32],title[96];
    sf_portal_get_credentials(ssid,sizeof(ssid),pwd,sizeof(pwd));
    char sub[40]="";
    if(want==FIRST_SETUP) snprintf(title,sizeof(title),"SkyFrame Setup");
    else if(want==CANT_JOIN) {
        snprintf(title,sizeof(title),"Can't join Wi-Fi");
        /* own line, never pushes into the QR */
        if(sf_config_network_count(&cfg)>1) snprintf(sub,sizeof(sub),"any of %d saved networks",sf_config_network_count(&cfg));
        else snprintf(sub,sizeof(sub),"\"%.32s\"",cfg.wifi[0].ssid);
    }
    else snprintf(title,sizeof(title),"SkyFrame Settings");
    sf_setup_info_t info={.title=title,.subtitle=sub[0]?sub:NULL,.ap_ssid=ssid,.ap_password=pwd,
                          .home_ip=ip[0]?ip:NULL,.closable=(want==SETTINGS)};
    sf_setup_show(true,&info);
}

/* ---- Clock and screen brightness -------------------------------------------------
   UTC from internet time (SNTP) plus the local offset Open-Meteo reports for the radar
   center, so daylight saving follows the weather data. */
static bool local_time(struct tm *out) {
    if(!last_weather.valid||!last_weather.has_utc_offset) return false;
    time_t now=time(NULL);
    if(now<1700000000) return false;            /* not synced yet */
    now+=last_weather.utc_offset_s;
    gmtime_r(&now,out);
    return true;
}
static int clock_minutes(const char *hhmm) {
    int h,m;
    return (hhmm&&sscanf(hhmm,"%d:%d",&h,&m)==2&&h>=0&&h<24&&m>=0&&m<60)?h*60+m:-1;
}
static bool in_window(int m,int start,int end) {   /* [start,end), may wrap past midnight */
    return start<=end?(m>=start&&m<end):(m>=start||m<end);
}
/* Day: normal. Sunset to sunrise: dimmed. Night hours: a soft glow; a touch brightens it
   until a minute passes without touches. */
/* Pull-down panel state: brightness (percent of the automatic level, saved), pause. */
#define PAUSE_MS 3600000U                /* a paused rotation resumes by itself after an hour */
static int bright_pct=100;
static bool backlight_dirty;             /* slider moved: apply now, not at the next second */
static bool rotation_paused;
static uint32_t paused_since;
static char clock_text[16];
static int wifi_bars=-1;
static void quick_refresh(void) {
    /* "4:52 PM  *  (wifi)", or "No Wi-Fi" */
    char status[64];
    snprintf(status,sizeof(status),"%s%s%s%s",clock_text,clock_text[0]?"  \xE2\x80\xA2  ":"",
             wifi_bars>=0?LV_SYMBOL_WIFI:"No Wi-Fi",rotation_paused?"  \xE2\x80\xA2  Paused":"");
    sf_quick_state_t q={.brightness=bright_pct,.paused=rotation_paused,.today_available=today&&today->planes,
                        .usb_available=sf_sd_usb_host_connected()&&!sf_sd_usb_active(),
                        .transitions=!(cfg.ui_flags&SF_UI_NO_TRANSITIONS),
                        .screen=ctrl.scheduler.screen,.status=status};
    sf_quick_update(&q);
}
static void quick_brightness(int pct, bool done) {
    bright_pct=pct<10?10:pct>100?100:pct;
    backlight_dirty=true;
    if(!done) return;
    nvs_handle_t h;   /* saved once, on release */
    if(nvs_open("skyframe",NVS_READWRITE,&h)==ESP_OK) {
        if(nvs_set_u8(h,"brightness",(uint8_t)bright_pct)==ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}
static void load_brightness(void) {
    nvs_handle_t h;
    uint8_t v=100;
    if(nvs_open("skyframe",NVS_READONLY,&h)==ESP_OK) { nvs_get_u8(h,"brightness",&v); nvs_close(h); }
    bright_pct=v<10?10:v>100?100:v;
}
static void open_settings(void);
static void quick_action(sf_quick_action_t a) {
    uint32_t t=now_ms();
    switch(a) {
    case SF_QUICK_RADAR:   sf_schedule_jump(&ctrl.scheduler,SF_SCREEN_RADAR,t); break;
    case SF_QUICK_WEATHER: sf_schedule_jump(&ctrl.scheduler,SF_SCREEN_WEATHER,t); break;
    case SF_QUICK_PHOTOS:  sf_schedule_jump(&ctrl.scheduler,SF_SCREEN_PHOTOS,t); break;
    case SF_QUICK_TODAY:   if(today && today->planes) sf_schedule_jump(&ctrl.scheduler,SF_SCREEN_TODAY,t); break;
    case SF_QUICK_PAUSE:   rotation_paused=!rotation_paused; paused_since=t; break;
    case SF_QUICK_SETTINGS: open_settings(); break;
    case SF_QUICK_USB:      sf_sd_usb_share(true); break;   /* the USB notice appears once the computer has it */
    case SF_QUICK_TRANSITIONS:   /* the switch: saved at once, no restart needed */
        cfg.ui_flags^=SF_UI_NO_TRANSITIONS;
        if(sf_config_nvs_save(&cfg)!=ESP_OK) ESP_LOGW(TAG,"Couldn't save the page animations setting");
        break;
    }
    quick_refresh();
}

/* USB notice's Done: take the card back (in the USB test, just end the test). */
static void usb_done(void) {
    if(test_is(SF_TEST_USB)) { end_test(); return; }
    sf_sd_usb_share(false);
}

/* Startup screen: shown from power-on until Wi-Fi joins (at least a moment, at most 20 s),
   or at once out of the way when setup needs the screen. */
#define SPLASH_MIN_MS 2500U
#define SPLASH_MAX_MS 20000U
static bool boot_splash;
static uint32_t splash_since, connected_since;
static void start_splash(void) {
    char version[64], built[32];
    build_name(version,sizeof(version),built,sizeof(built));
    sf_splash_show(version+9);   /* without the leading "SkyFrame ", shown big above it */
    sf_splash_status("Starting...");
    splash_since=now_ms();
    connected_since=0;
    if(!test_mode) boot_splash=true;
}
static void tick_splash(uint32_t t) {
    if(!boot_splash) return;
    uint32_t up=t-splash_since;
    bool connected=sf_wifi_is_connected();
    if(connected && !connected_since) connected_since=t?t:1;
    if(setup_mode || sf_wifi_needs_setup() || up>=SPLASH_MAX_MS ||
       (connected && up>=SPLASH_MIN_MS && (uint32_t)(t-connected_since)>=700)) {
        boot_splash=false;
        sf_splash_hide();
        return;
    }
    sf_splash_status(connected?"Connected":up<600?"Starting...":"Joining Wi-Fi...");
}

/* ---- Over-the-air updates ---------------------------------------------------------
   A newly installed version keeps itself once it has been on Wi-Fi for a minute; one that
   never joins within 15 minutes goes back to the previous version (a crash or freeze before
   that is caught by the bootloader). Updates are checked in the 3 AM hour, while dimmed. */
static int local_ymd(const struct tm *lt);
#define UPDATE_PROVE_MS 60000U
#define UPDATE_GIVE_UP_MS (15U*60000U)
static void tick_updates(uint32_t t) {
    static uint32_t last, wifi_since;
    static int checked_day;
    static bool news_done;
    if(last && (uint32_t)(t-last)<1000) return;
    last=t?t:1;
    bool online=sf_wifi_is_connected();
    if(!online) wifi_since=0;
    else if(!wifi_since) wifi_since=t?t:1;
    if(sf_update_pending_verify()) {
        if(online && (uint32_t)(t-wifi_since)>=UPDATE_PROVE_MS) sf_update_confirm();
        else if(!online && t>=UPDATE_GIVE_UP_MS) sf_update_reject();
        return;
    }
    /* What's new, once, when nothing covers the screens. */
    if(!news_done && !boot_splash && !sf_splash_visible() && !setup_mode && !setup_up && !test_mode && !sf_sighting_visible()) {
        news_done=true;
        char v[16], notes[160], title[32];
        if(sf_update_take_news(v,sizeof(v),notes,sizeof(notes))) {
            snprintf(title,sizeof(title),"UPDATED TO v%.16s",v);
            sf_sighting_view_t view={.kind=SF_SIGHTING_NEWS,.title=title,.name=notes[0]?notes:"A new version of SkyFrame",
                                     .detail="Installed by itself, nothing to do",.seconds=15};
            sf_sighting_show(&view);
        }
    }
    struct tm lt;
    if(online && !test_mode && !setup_mode && local_time(&lt) && lt.tm_hour==UPDATE_HOUR) {
        int day=local_ymd(&lt);
        if(day!=checked_day) { checked_day=day; ESP_LOGI(TAG,"Nightly update check"); sf_update_check_now(true); }
    }
}

static void update_clock_and_backlight(uint32_t t) {
    static uint32_t last;
    bool second=!last || (uint32_t)(t-last)>=1000;
    if(!second && !backlight_dirty) return;
    backlight_dirty=false;
    struct tm lt;
    bool known=local_time(&lt);
    char text[16]="";
    if(second) {   /* clock, Wi-Fi bars and the panel's status: once a second (not per slider step) */
    last=t?t:1;
    if(known) {
        snprintf(text,sizeof(text),"%d:%02d %s",lt.tm_hour%12?lt.tm_hour%12:12,lt.tm_min,lt.tm_hour>=12?"PM":"AM");
        today_year=lt.tm_year+1900;
        birthdays_set_today(today_year,lt.tm_mon+1,lt.tm_mday);
    }
    skyframe_ui_set_clock(text);
    sf_weather_set_clock(text);
    snprintf(clock_text,sizeof(clock_text),"%s",text);
    /* Radar Wi-Fi icon: bars from the signal strength, like a phone's. */
    wifi_ap_record_t ap;
    int bars=-1;
    if(sf_wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap)==ESP_OK)
        bars=ap.rssi>=-55?4:ap.rssi>=-67?3:ap.rssi>=-75?2:1;
    skyframe_ui_set_wifi(bars);
    wifi_bars=bars;
    quick_refresh();
    }

    int level=BRIGHT_DAY;
    bool night=false;
    bool attention=setup_mode||settings_open||sf_wifi_needs_setup()||sf_sd_usb_active()||
                   (test_mode&&!test_is(SF_TEST_NIGHT));
    if(known && !attention) {
        int m=lt.tm_hour*60+lt.tm_min;
        int rise=clock_minutes(last_weather.sunrise), set=clock_minutes(last_weather.sunset);
        if(rise>=0 && set>=0 && !in_window(m,rise,set)) level=BRIGHT_EVENING;
        int start=(int)cfg.night_start_hour*60, end=(int)cfg.night_end_hour*60;
        if(start!=end && in_window(m,start,end)) night=true;
    }
    if(night||test_is(SF_TEST_NIGHT)) {
        /* The night-mode test dims at once: the tap that started it doesn't count. */
        uint32_t window=WAKE_MS;
        if(test_is(SF_TEST_NIGHT) && (uint32_t)(t-test_started)<window) window=t-test_started;
        level=lv_disp_get_inactive_time(NULL)<window?BRIGHT_WAKE:BRIGHT_NIGHT;
    }
    /* The panel's brightness scales the automatic level (not while setup or settings need reading). */
    if(!attention) { level=(level*bright_pct+50)/100; if(level<1) level=1; }
    static int applied=-1;
    if(level!=applied){ Set_Backlight((uint8_t)level); applied=level; }
}

/* ---- Shared text helpers -------------------------------------------------------- */
static int local_ymd(const struct tm *lt) { return (lt->tm_year+1900)*10000+(lt->tm_mon+1)*100+lt->tm_mday; }
static const char *compass8(float deg) {
    static const char *const n[8]={"N","NE","E","SE","S","SW","W","NW"};
    return n[((int)lroundf(fmodf(deg+360.f,360.f)/45.f))%8];
}
/* "BOEING 747-8" -> "Boeing 747-8": words with digits stay as they are. Type code if none. */
static void nice_type(const char *desc, const char *code, char *out, size_t cap) {
    if(!desc||!desc[0]) { snprintf(out,cap,"%s",code?code:""); return; }
    snprintf(out,cap,"%s",desc);
    for(char *w=out;*w;) {
        while(*w==' '||*w=='-'||*w=='/') w++;
        char *e=w; bool digits=false;
        while(*e && *e!=' ' && *e!='/') { if(isdigit((unsigned char)*e)) digits=true; e++; }
        if(!digits && e>w) for(char *c=w+1;c<e;c++) *c=(char)tolower((unsigned char)*c);
        w=e;
    }
}
static void altitude_text(int ft, char *out, size_t cap) {
    if(ft>=1000) snprintf(out,cap,"%d,%03d ft",ft/1000,ft%1000);
    else snprintf(out,cap,"%d ft",ft<0?0:ft);
}

/* ---- Severe-weather alerts (National Weather Service) ---------------------------
   The latest list from the service; alerts are dropped from the card once they end, even
   if the next check fails. A test shows two sample alerts instead. */
static sf_wx_alerts_t *wx_alerts, *wx_work;   /* PSRAM */
static void take_wx_alerts(const sf_wx_alerts_t *a) {
    if(!wx_alerts) return;
    if(a->total!=wx_alerts->total || (a->count && strcmp(a->alert[0].event,wx_alerts->alert[0].event)))
        ESP_LOGI(TAG,"Weather alerts: %d in effect%s%s",a->total,a->count?", first: ":"",a->count?a->alert[0].event:"");
    if(memcmp(wx_alerts,a,sizeof(*a))) { *wx_alerts=*a; wx_alerts_changed=true; }
}
static int weekday_of(int ymd) {   /* 0 = Sunday */
    static const int k[]={0,3,2,5,0,3,5,1,4,6,2,4};
    int y=ymd/10000, m=ymd/100%100, d=ymd%100;
    if(m<1||m>12) return 0;
    if(m<3) y--;
    return (y+y/4-y/100+y/400+k[m-1]+d)%7;
}
static void until_text(const sf_wx_alert_t *a, int today_ymd, char *out, size_t cap) {
    static const char *const days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    out[0]=0;
    if(a->ends_minute<0) return;
    int h=a->ends_minute/60, m=a->ends_minute%60;
    char day[8]="";
    if(a->ends_date && a->ends_date!=today_ymd) snprintf(day,sizeof(day),"%s ",days[weekday_of(a->ends_date)]);
    snprintf(out,cap,"Until %s%d:%02d %s",day,h%12?h%12:12,m,h>=12?"PM":"AM");
}
static void sample_wx_alerts(sf_wx_alerts_t *w, const struct tm *lt) {
    memset(w,0,sizeof(*w));
    w->count=w->total=2;
    sf_wx_alert_t *a=&w->alert[0];
    snprintf(a->event,sizeof(a->event),"Severe Thunderstorm Warning");
    snprintf(a->area,sizeof(a->area),"Your area (test: no real alert is in effect)");
    snprintf(a->text,sizeof(a->text),
        "* WHAT...Severe thunderstorms with wind gusts up to 60 mph and quarter size hail.\n"
        "* IMPACTS...Hail damage to vehicles is expected. Expect wind damage to roofs, siding, and trees.\n"
        "For your protection move to an interior room on the lowest floor of a building.\n"
        "This is a SkyFrame test. No real alert is in effect.");
    a->level=3;
    a->ends_minute=lt?(lt->tm_hour*60+lt->tm_min+45)%1440:-1;
    a->ends_date=lt?local_ymd(lt):0;
    a=&w->alert[1];
    snprintf(a->event,sizeof(a->event),"Flood Watch");
    snprintf(a->text,sizeof(a->text),"* WHAT...Flooding caused by excessive rainfall is possible.\n"
        "This is a SkyFrame test. No real alert is in effect.");
    a->level=2;
    a->ends_minute=6*60;
    a->ends_date=0;
}
static void tick_wx_alerts(uint32_t t) {
    static uint32_t last, shown_sig;
    if(!wx_alerts||!wx_work) return;
    if(!wx_alerts_changed && last && (uint32_t)(t-last)<30000) return;   /* expiry check every 30 s */
    last=t?t:1;
    bool force=wx_alerts_changed;
    wx_alerts_changed=false;
    struct tm lt;
    bool known=local_time(&lt);
    int today_ymd=known?local_ymd(&lt):0;
    sf_wx_alerts_t *w=wx_work;
    if(test_is(SF_TEST_WX_ALERT)) sample_wx_alerts(w,known?&lt:NULL);
    else {
        memset(w,0,sizeof(*w));
        time_t now=time(NULL);
        for(int i=0;i<wx_alerts->count;i++) {
            const sf_wx_alert_t *a=&wx_alerts->alert[i];
            if(a->ends_utc && now>1700000000 && (int64_t)now>=a->ends_utc) continue;   /* over */
            w->alert[w->count++]=*a;
        }
        w->total=wx_alerts->total-(wx_alerts->count-w->count);
    }
    /* Redraw only when what the card says changes (the day matters for "Until Mon ..."). */
    uint32_t sig=2166136261u;
    #define MIX(v) (sig=(sig^(uint32_t)(v))*16777619u)
    MIX(w->count); MIX(w->total); MIX(today_ymd);
    for(int i=0;i<w->count;i++) {
        for(const char *c=w->alert[i].event;*c;c++) MIX(*c);
        MIX(w->alert[i].ends_minute); MIX(w->alert[i].ends_date); MIX(w->alert[i].text[0]);
    }
    #undef MIX
    if(!force && sig==shown_sig) return;
    shown_sig=sig;
    sf_weather_alert_view_t v[SF_MAX_WX_ALERTS];
    char until[SF_MAX_WX_ALERTS][32];
    for(int i=0;i<w->count;i++) {
        until_text(&w->alert[i],today_ymd,until[i],sizeof(until[i]));
        v[i]=(sf_weather_alert_view_t){.event=w->alert[i].event,.until=until[i],.area=w->alert[i].area,
                                       .text=w->alert[i].text,.level=w->alert[i].level};
    }
    sf_weather_set_alerts(v,w->count,w->total);
}

/* ---- Today's sky -----------------------------------------------------------------
   Every aircraft of every snapshot is counted once per local day; the page joins the
   rotation once the day's first plane is seen and starts over at midnight. */
static int today_hour=-1;
/* The hero photo: the rarest type's picture from the card (/planes/types), loaded by a
   worker task (card reads and JPEG decoding take a moment) and handed over by message. */
static portMUX_TYPE today_photo_lock=portMUX_INITIALIZER_UNLOCKED;
static char today_photo_type[8];          /* the type wanted on the page ("" = none) */
static TaskHandle_t today_photo_task;
static void request_today_photo(const char *type) {
    bool changed;
    taskENTER_CRITICAL(&today_photo_lock);
    changed=strcmp(today_photo_type,type)!=0;
    if(changed) snprintf(today_photo_type,sizeof(today_photo_type),"%s",type);
    taskEXIT_CRITICAL(&today_photo_lock);
    if(changed && today_photo_task) xTaskNotifyGive(today_photo_task);
}
static void today_photo_worker(void *arg) {
    (void)arg;
    app_msg_t *msg=heap_caps_calloc(1,sizeof(*msg),MALLOC_CAP_SPIRAM);
    char loaded[8]="\x01";                  /* nothing loaded yet: differs from any type */
    for(;;) {
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(5000));
        char type[8];
        taskENTER_CRITICAL(&today_photo_lock);
        memcpy(type,today_photo_type,sizeof(type));
        taskEXIT_CRITICAL(&today_photo_lock);
        if(!msg || !strcmp(type,loaded)) continue;
        uint16_t *px=type[0]?plane_library_load_type(type):NULL;
        memset(msg,0,sizeof(*msg));
        msg->type=MSG_TODAY_PHOTO;
        snprintf(msg->data.photo.icao,sizeof(msg->data.photo.icao),"%s",type);
        msg->data.photo.pixels=px;
        if(send_message(msg)) snprintf(loaded,sizeof(loaded),"%s",type);
        else free(px);
    }
}
static void count_today(const sf_aircraft_result_t *r) {
    struct tm lt;
    if(!today || !local_time(&lt)) return;
    int day=local_ymd(&lt);
    bool changed=false;
    for(size_t i=0;i<r->count;i++) changed|=sf_today_add(today,&r->planes[i],day,lt.tm_hour);
    if(changed) refresh_today();
}
static void pick_who(const sf_today_pick_t *p, char *out, size_t cap) {
    snprintf(out,cap,"%s%s%s",p->who,p->type_code[0]?"  ":"",p->type_code);
}
static void refresh_today(void) {
    if(!today) return;
    sf_schedule_set_today(&ctrl.scheduler,today->planes?cfg.today_seconds:0);
    if(test_is(SF_TEST_TODAY)) return;   /* the sample stays until the test ends */
    static const char *const wd[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char *const mon[]={"January","February","March","April","May","June","July","August",
                                    "September","October","November","December"};
    struct tm lt;
    bool known=local_time(&lt);
    char date[40]="Today";
    if(known) snprintf(date,sizeof(date),"%s, %s %d",wd[lt.tm_wday%7],mon[lt.tm_mon%12],lt.tm_mday);
    today_hour=known?lt.tm_hour:-1;
    char hi[16]="",hi_who[24]="",cl[16]="",cl_who[24]="",fa[16]="",fa_who[24]="";
    char rare[72]="",special[64]="",special_who[96]="";
    if(today->highest.who[0]) { altitude_text((int)today->highest.value,hi,sizeof(hi)); pick_who(&today->highest,hi_who,sizeof(hi_who)); }
    if(today->closest.who[0]) { snprintf(cl,sizeof(cl),"%.1f mi",today->closest.value); pick_who(&today->closest,cl_who,sizeof(cl_who)); }
    if(today->fastest.who[0]) { snprintf(fa,sizeof(fa),"%.0f mph",today->fastest.value*1.15078f); pick_who(&today->fastest,fa_who,sizeof(fa_who)); }
    const sf_today_type_t *r=sf_today_rarest(today);
    char hero[40]="";
    if(r) {
        nice_type(r->desc,r->type_code,hero,sizeof(hero));
        snprintf(rare,sizeof(rare),"%s  \xE2\x80\xA2  %s",hero,r->who);
    }
    request_today_photo(r?r->type_code:"");
    int n=0;
    #define PART(cnt,word) if((cnt) && n<(int)sizeof(special)) \
        n+=snprintf(special+n,sizeof(special)-n,"%s%u %s",n?"  \xE2\x80\xA2  ":"",(unsigned)(cnt),word)
    PART(today->vip,"VIP");
    PART(today->military,"military");
    PART(today->emergency,today->emergency==1?"emergency":"emergencies");
    #undef PART
    n=0;
    for(unsigned i=0;i<today->notable_count && n<(int)sizeof(special_who);i++)
        n+=snprintf(special_who+n,sizeof(special_who)-n,"%s%s%s%s%s",i?", ":"",today->notable[i].who,
                    today->notable[i].type_code[0]?" (":"",today->notable[i].type_code,today->notable[i].type_code[0]?")":"");
    sf_today_view_t v={.date=date,.planes=today->planes,.now_hour=today_hour,.highest=hi,.highest_who=hi_who,
        .closest=cl,.closest_who=cl_who,.fastest=fa,.fastest_who=fa_who,.rarest=rare,.special=special,.special_who=special_who,
        .hero_tag="RAREST TODAY",.hero_name=hero,.hero_who=r?r->who:""};
    memcpy(v.per_hour,today->per_hour,sizeof(v.per_hour));
    sf_today_set(&v);
}
/* Midnight starts a new day; the hour bar moves on. */
static void tick_today(uint32_t t) {
    static uint32_t last;
    if(!today || (last && (uint32_t)(t-last)<1000)) return;
    last=t?t:1;
    struct tm lt;
    if(!local_time(&lt)) return;
    int day=local_ymd(&lt);
    if(today->day && today->day!=day) { sf_today_reset(today,day); refresh_today(); }
    else if(lt.tm_hour!=today_hour) refresh_today();
}
static void show_sample_today(void) {
    static const uint16_t hours[24]={0,0,0,0,0,1,4,9,12,10,8,9,11,10,9,12,14,13,9,6,4,3,1,0};
    struct tm lt;
    sf_today_view_t v={.date="Sample day",.planes=155,.now_hour=local_time(&lt)?lt.tm_hour:16,
        .highest="41,000 ft",.highest_who="DAL1432  A321",.closest="0.6 mi",.closest_who="N512SP  C172",
        .fastest="583 mph",.fastest_who="UPS2961  B763",.rarest="Boeing 747-8F  \xE2\x80\xA2  CLX4421",
        .special="1 VIP  \xE2\x80\xA2  2 military",.special_who="SAM44 (C32), RCH451 (C17), CONVOY12 (C130)",
        .hero_tag="RAREST TODAY",.hero_name="Boeing 747-8F",.hero_who="CLX4421"};
    memcpy(v.per_hour,hours,sizeof(hours));
    sf_today_set(&v);
    request_today_photo("B748");
}

/* ---- Sighting alerts -------------------------------------------------------------
   Emergencies, VIP and military flights and the types chosen on the settings page pop a
   banner over any screen. Each aircraft alerts once per 6 hours (again if it becomes an
   emergency). Banners wait while a full-screen notice or the test menu is up. */
typedef struct { char icao[9]; uint8_t kind; uint32_t at; } sighted_t;
typedef struct { sf_sighting_kind_t kind; char title[32], name[64], detail[48]; uint32_t at; unsigned seconds; } sighting_t;
#define SIGHTED_SLOTS 32
#define PENDING_SIGHTINGS 4
static sighted_t sighted[SIGHTED_SLOTS];
static unsigned sighted_next;
static sighting_t pending[PENDING_SIGHTINGS];
static int pending_count;
static bool sighting_of(const sf_plane_t *p, sf_sighting_kind_t *kind, char *title, size_t cap) {
    sf_special_t sp=sf_plane_special(p);
    if(sp==SF_SPECIAL_EMERGENCY && (cfg.alert_flags&SF_ALERT_EMERGENCY)) {
        *kind=SF_SIGHTING_EMERGENCY;
        if(!strcmp(p->squawk,"7700")) snprintf(title,cap,"EMERGENCY  7700");
        else if(!strcmp(p->squawk,"7600")) snprintf(title,cap,"RADIO FAILURE  7600");
        else if(!strcmp(p->squawk,"7500")) snprintf(title,cap,"HIJACK CODE  7500");
        else {
            snprintf(title,cap,"EMERGENCY  %.12s",p->emergency);
            for(char *c=title;*c;c++) *c=(char)toupper((unsigned char)*c);
        }
        return true;
    }
    if(sp==SF_SPECIAL_VIP && (cfg.alert_flags&SF_ALERT_VIP)) {
        *kind=SF_SIGHTING_VIP;
        snprintf(title,cap,"%s",!strcmp(p->callsign,"AF1")?"AIR FORCE ONE":!strcmp(p->callsign,"AF2")?"AIR FORCE TWO":
                 !strcmp(p->type_code,"VC25")?"PRESIDENTIAL 747":"GOVERNMENT VIP FLIGHT");
        return true;
    }
    if(sp==SF_SPECIAL_MILITARY && (cfg.alert_flags&SF_ALERT_MILITARY) && !sf_is_helicopter(p)) {
        *kind=SF_SIGHTING_MILITARY;
        snprintf(title,cap,"MILITARY AIRCRAFT");
        return true;
    }
    if(sf_type_list_match(cfg.alert_types,p->type_code)) {
        *kind=SF_SIGHTING_TYPE;
        snprintf(title,cap,"SPOTTED  %.7s",p->type_code);
        return true;
    }
    return false;
}
static void queue_sighting(const sighting_t *s) {
    if(pending_count<PENDING_SIGHTINGS) { pending[pending_count++]=*s; return; }
    int low=0;   /* full: replace the least important if this one matters more */
    for(int k=1;k<PENDING_SIGHTINGS;k++) if(pending[k].kind<pending[low].kind) low=k;
    if(pending[low].kind<s->kind) pending[low]=*s;
}
/* "Overhead now": airborne and within OVERHEAD_MILES, or passing that close before the next
   traffic update (closest approach along its track; snapshots are 20 s apart, in which a
   jet covers more than a mile). */
#define OVERHEAD_MILES 1.0f
#define OVERHEAD_LOOKAHEAD_S 25.f
#define OVERHEAD_REPEAT_MS (30U*60000U)
#define OVERHEAD_SECONDS 8
static bool passes_overhead(const sf_plane_t *p) {
    if(p->on_ground) return false;
    if(p->distance_miles<=OVERHEAD_MILES) return true;
    if(!p->has_speed || !p->has_heading || p->speed_knots<30.f) return false;
    float b=p->bearing_deg*(float)(PI/180.0), h=p->heading_deg*(float)(PI/180.0);
    float x=sinf(b)*p->distance_miles, y=cosf(b)*p->distance_miles;   /* east, north */
    float v=p->speed_knots*1.15078f/3600.f;                          /* miles per second */
    float vx=sinf(h)*v, vy=cosf(h)*v;
    float tt=-(x*vx+y*vy)/(vx*vx+vy*vy);
    if(tt<0.f || tt>OVERHEAD_LOOKAHEAD_S) return false;
    return hypotf(x+vx*tt,y+vy*tt)<=OVERHEAD_MILES;
}
typedef struct { char icao[9]; uint32_t at; } overhead_seen_t;
static overhead_seen_t overhead_seen[16];
static unsigned overhead_next;
static bool overhead_new(const char *icao, uint32_t t) {
    for(int k=0;k<16;k++)
        if(!strcmp(overhead_seen[k].icao,icao) && (uint32_t)(t-overhead_seen[k].at)<OVERHEAD_REPEAT_MS) return false;
    overhead_seen_t *e=&overhead_seen[overhead_next++%16];
    snprintf(e->icao,sizeof(e->icao),"%s",icao);
    e->at=t;
    return true;
}
static void check_sightings(const sf_aircraft_result_t *r) {
    uint32_t t=now_ms();
    /* Forget entries past their window: on the 49-day wrapping clock an old one would
       otherwise look recent again one day. */
    for(int k=0;k<SIGHTED_SLOTS;k++) if((uint32_t)(t-sighted[k].at)>=SIGHTING_REPEAT_MS) sighted[k].icao[0]=0;
    for(int k=0;k<16;k++) if((uint32_t)(t-overhead_seen[k].at)>=OVERHEAD_REPEAT_MS) overhead_seen[k].icao[0]=0;
    for(size_t i=0;i<r->count;i++) {
        const sf_plane_t *p=&r->planes[i];
        if(!p->icao[0]) continue;
        const char *who=p->callsign[0]?p->callsign:p->registration[0]?p->registration:p->icao;
        char type[40], alt[16]="";
        nice_type(p->aircraft_type,p->type_code,type,sizeof(type));
        if(p->on_ground) snprintf(alt,sizeof(alt),"on the ground");
        else if(p->has_altitude) altitude_text(p->altitude_ft,alt,sizeof(alt));
        sighting_t s={.at=t,.seconds=SIGHTING_SECONDS};
        snprintf(s.name,sizeof(s.name),"%s%s%s",who,type[0]?"  \xE2\x80\xA2  ":"",type);

        char title[32];
        if(sighting_of(p,&s.kind,title,sizeof(title))) {
            sighted_t *slot=NULL;
            for(int k=0;k<SIGHTED_SLOTS;k++) if(!strcmp(sighted[k].icao,p->icao)) slot=&sighted[k];
            if(slot && slot->kind>=s.kind && (uint32_t)(t-slot->at)<SIGHTING_REPEAT_MS) continue;
            if(!slot) slot=&sighted[sighted_next++%SIGHTED_SLOTS];
            snprintf(slot->icao,sizeof(slot->icao),"%s",p->icao);
            slot->kind=(uint8_t)s.kind; slot->at=t;
            snprintf(s.title,sizeof(s.title),"%s",title);
            snprintf(s.detail,sizeof(s.detail),p->distance_miles<10.f?"%.1f mi %s%s%s":"%.0f mi %s%s%s",
                     p->distance_miles,compass8(p->bearing_deg),alt[0]?"  \xE2\x80\xA2  ":"",alt);
            ESP_LOGI(TAG,"Sighting: %s, %s (%s)",title,who,p->icao);
            queue_sighting(&s);
            continue;
        }
        /* An ordinary plane passing over: a short card. */
        if((cfg.alert_flags&SF_ALERT_NO_OVERHEAD) || !passes_overhead(p) || !overhead_new(p->icao,t)) continue;
        s.kind=SF_SIGHTING_OVERHEAD;
        s.seconds=OVERHEAD_SECONDS;
        snprintf(s.title,sizeof(s.title),"OVERHEAD NOW");
        char speed[16]="";
        if(p->has_speed) snprintf(speed,sizeof(speed),"%.0f mph",p->speed_knots*1.15078f);
        /* "3,800 ft  *  heading N  *  210 mph", leaving out what isn't known */
        int n=snprintf(s.detail,sizeof(s.detail),"%s",alt);
        if(p->has_heading && n<(int)sizeof(s.detail))
            n+=snprintf(s.detail+n,sizeof(s.detail)-n,"%sheading %s",n?"  \xE2\x80\xA2  ":"",compass8(p->heading_deg));
        if(speed[0] && n<(int)sizeof(s.detail))
            snprintf(s.detail+n,sizeof(s.detail)-n,"%s%s",n?"  \xE2\x80\xA2  ":"",speed);
        ESP_LOGI(TAG,"Overhead: %s (%s), %.1f mi",who,p->icao,p->distance_miles);
        queue_sighting(&s);
    }
}
static void tick_sightings(uint32_t t, bool blocked) {
    if(blocked || !pending_count || sf_sighting_visible()) return;
    int best=0;
    for(int k=1;k<pending_count;k++) if(pending[k].kind>pending[best].kind) best=k;
    sighting_t s=pending[best];
    pending[best]=pending[--pending_count];
    /* Held back too long (a notice covered the screen): old news. "Overhead now" only lasts a moment. */
    if((uint32_t)(t-s.at)>(s.kind==SF_SIGHTING_OVERHEAD?30000u:10*60000u)) return;
    sf_sighting_view_t v={.kind=s.kind,.title=s.title,.name=s.name,.detail=s.detail,.seconds=s.seconds};
    sf_sighting_show(&v);
    sample_banner=false;
}
static void show_sample_overhead(void) {
    sf_sighting_view_t v={.kind=SF_SIGHTING_OVERHEAD,.title="OVERHEAD NOW",.name="SWA2134  \xE2\x80\xA2  Boeing 737-800",
                          .detail="3,800 ft  \xE2\x80\xA2  heading N  \xE2\x80\xA2  210 mph",.seconds=OVERHEAD_SECONDS};
    sf_sighting_show(&v);
    sample_banner=true;
}
static void show_sample_sighting(void) {
    sf_sighting_view_t v={.kind=SF_SIGHTING_VIP,.title="AIR FORCE ONE",.name="AF1  \xE2\x80\xA2  Boeing VC-25A",
                          .detail="8.4 mi NE  \xE2\x80\xA2  21,000 ft",.seconds=SIGHTING_SECONDS};
    sf_sighting_show(&v);
    sample_banner=true;
}
/* Tapped: the radar, where the aircraft is. In the test, the sample traffic (with its
   Air Force One) stands in for the real radar. */
static void sighting_tapped(void) {
    if(test_is(SF_TEST_SIGHTING) || test_is(SF_TEST_OVERHEAD)) { test_action(SF_TEST_TRAFFIC); return; }
    sf_schedule_jump(&ctrl.scheduler,SF_SCREEN_RADAR,now_ms());
}

void app_main(void) {
    /* NVS is initialized before Wi-Fi; no personal details built into firmware. */
    ESP_ERROR_CHECK(sf_config_nvs_init());
    sf_update_init(sf_planned_restart);   /* rollback bookkeeping and the crash report, first thing */
    sf_update_crash_summary(crash_text,sizeof(crash_text));
    if(sf_config_nvs_load(&cfg)!=ESP_OK) {sf_config_defaults(&cfg);setup_mode=true;}
    else setup_mode=!cfg.wifi[0].ssid[0];

    /* Preserve Waveshare's original order to honor shared LCD/SD lines. */
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    SD_Init();
    /* Mounts the card for SkyFrame and enables USB file transfer on the native port. */
    if(SD_Card) sf_sd_start(SD_Card);
    else ESP_LOGW(TAG,"No microSD card detected; slideshow and offline plane photos unavailable");
    LVGL_Init();

    skyframe_ui_init(lv_scr_act());
    sf_screens_init(lv_scr_act());
    skyframe_ui_set_radius((float)cfg.display_miles);
    skyframe_ui_set_location(cfg.latitude,cfg.longitude);
    skyframe_ui_set_frame_clock(LCD_Vsync_Count);
    skyframe_ui_set_select_cb(selected_plane);
    sf_screens_set_long_press_cb(cycle_screen);
    sf_screens_set_hold_cb(open_settings);
    sf_screens_set_setup_close_cb(close_settings);
    sf_screens_set_birthday_dismiss_cb(dismiss_birthday);
    sf_screens_set_test_cb(enter_test_mode,test_action);
    sf_screens_set_sighting_cb(sighting_tapped);
    sf_screens_set_usb_done_cb(usb_done);
    sf_screens_set_frame_source(LCD_Frame_Buffer);
    sf_quick_set_callbacks(quick_brightness,quick_action);
    load_brightness();
    wx_alerts=heap_caps_calloc(1,sizeof(*wx_alerts),MALLOC_CAP_SPIRAM);
    wx_work=heap_caps_calloc(1,sizeof(*wx_work),MALLOC_CAP_SPIRAM);
    today=heap_caps_calloc(1,sizeof(*today),MALLOC_CAP_SPIRAM);
    float airport_dist=0,airport_dir=0;
    sf_geo(cfg.latitude,cfg.longitude,SF_BWI_LATITUDE,SF_BWI_LONGITUDE,&airport_dist,&airport_dir);
    skyframe_ui_set_airport(airport_dist,airport_dir,true);
    sf_controller_init(&ctrl,now_ms());
    sf_controller_set_activation(&ctrl,(float)cfg.activation_miles);
    sf_schedule_set_durations(&ctrl.scheduler,cfg.radar_seconds,cfg.weather_seconds,cfg.photo_seconds);
    display_screen(SF_SCREEN_RADAR,false);

    if(sf_photoplayer_start()!=ESP_OK) ESP_LOGE(TAG,"Slideshow could not start");
    birthdays_start();

    start_splash();
    ESP_ERROR_CHECK(sf_wifi_begin(&cfg));
    setup_banner(now_ms());

    /* Messages are a few KB each; keep queues and caches in PSRAM, not scarce internal RAM. */
    mailbox=xQueueCreateWithCaps(5,sizeof(app_msg_t),MALLOC_CAP_SPIRAM);
    info_requests=xQueueCreateWithCaps(1,sizeof(info_req_t),MALLOC_CAP_SPIRAM);
    info_cache=heap_caps_calloc(ROUTE_CACHE_SLOTS,sizeof(*info_cache),MALLOC_CAP_SPIRAM);
    app_msg_t *msg=heap_caps_malloc(sizeof(*msg),MALLOC_CAP_SPIRAM);
    if(!mailbox||!info_requests||!msg)ESP_LOGE(TAG,"Insufficient memory for network queues");
    else {
        /* HTTPS handshakes are seconds of CPU; keep them on CPU1, away from LVGL and Wi-Fi on CPU0. */
        xTaskCreatePinnedToCore(network_task,"skyframe_net",12288,NULL,4,NULL,1);
        xTaskCreatePinnedToCore(info_task,"skyframe_info",10240,NULL,3,NULL,1);
        xTaskCreatePinnedToCore(today_photo_worker,"today_photo",6144,NULL,2,&today_photo_task,1);
    }

    ESP_LOGI(TAG,"SkyFrame v%s booted: radar=%u mi, trigger=%u mi",sf_update_version(),cfg.display_miles,cfg.activation_miles);
    last_motion_ms=now_ms();
    /* Unattended recovery: if this loop ever stalls, the task watchdog restarts the device. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while(1) {
        esp_task_wdt_reset();
        if(msg && mailbox) {
            for(int i=0;i<4&&xQueueReceive(mailbox,msg,0)==pdTRUE;i++) {
                if(msg->type==MSG_TRAFFIC)apply_aircraft(&msg->data.aircraft);
                else if(msg->type==MSG_WEATHER)show_weather(&msg->data.weather);
                else if(msg->type==MSG_INFO)skyframe_ui_update_info(&msg->data.info);
                else if(msg->type==MSG_WX_ALERTS)take_wx_alerts(&msg->data.alerts);
                else if(msg->type==MSG_TODAY_PHOTO) {
                    /* Still the photo the page wants? (The rarest type may have changed meanwhile.) */
                    uint16_t *px=msg->data.photo.pixels;
                    if(strcmp(msg->data.photo.icao,today_photo_type) || !sf_today_set_photo(px)) free(px);
                }
                else if(msg->type==MSG_VERSE) {
                    sf_verse_set(&msg->data.verse);
                    verse_ready=true;
                    update_optional_screens();
                }
                else if(msg->type==MSG_PHOTO) {
                    uint16_t *px=msg->data.photo.pixels;
                    if(!px) skyframe_ui_photo_unavailable(msg->data.photo.icao);
                    else if(!skyframe_ui_set_photo(msg->data.photo.icao,px)) free(px);
                }
            }
        }
        uint32_t t=now_ms();
        if(skyframe_ui_details_open()) {
            if(!details_timing){details_timing=true;details_since=t;}
            else if((uint32_t)(t-details_since)>=DETAILS_TIMEOUT_MS) skyframe_ui_close_details();
        } else details_timing=false;
        sf_controller_details(&ctrl,skyframe_ui_details_open());
        sf_screen_t scheduled=sf_controller_tick(&ctrl,t);
        /* A weather alert's full text is open: the rotation waits (up to the details timeout). */
        static bool wx_open_timing;
        static uint32_t wx_open_since;
        if(sf_weather_alert_open() && ctrl.scheduler.screen==SF_SCREEN_WEATHER) {
            if(!wx_open_timing) { wx_open_timing=true; wx_open_since=t; }
            else if((uint32_t)(t-wx_open_since)>=DETAILS_TIMEOUT_MS) sf_weather_close_alert();
            ctrl.scheduler.phase_start_ms=t;
        } else wx_open_timing=false;
        /* Paused from the pull-down panel (up to an hour), or the panel is open: stay on this screen. */
        if(rotation_paused && (uint32_t)(t-paused_since)>=PAUSE_MS) { rotation_paused=false; quick_refresh(); }
        if(rotation_paused || sf_quick_is_open()) sf_schedule_hold(&ctrl.scheduler,t);
        if(setup_mode) {
            /* First boot: leave instructions visible until reset after save; the screens
               underneath stay paused. */
            display_screen(active_screen,true);
        }
        else {
            sf_screen_t target=scheduled;
            sf_screen_t forced;
            if(test_tick(t,&forced)) target=forced;   /* a running test picks the screen */
            bool menu=test_mode&&test_running==TEST_NONE;
            bool covered=menu||setup_up||sf_sd_usb_active()||test_is(SF_TEST_USB);
            show_screen(target,covered);
            /* Planes move only on a visible radar; on uncovering they are placed at once. */
            if(covered) last_motion_ms=t-MOTION_STEP_MS;
            else if(active_screen==SF_SCREEN_RADAR && (uint32_t)(t-last_motion_ms)>=MOTION_STEP_MS){project_traffic(t);last_motion_ms=t;}
            sf_photo_tick(t);
        }
        setup_banner(t);
        /* While the card is open on a computer, say so above everything else. */
        sf_usb_show(sf_sd_usb_active()||test_is(SF_TEST_USB));
        update_clock_and_backlight(t);
        tick_splash(t);
        tick_updates(t);
        tick_wx_alerts(t);
        tick_today(t);
        /* Sighting banners wait while something covers the screens. */
        tick_sightings(t,setup_mode||setup_up||(test_mode&&test_running==TEST_NONE)||sf_sd_usb_active()||test_is(SF_TEST_USB)||
                         test_is(SF_TEST_SIGHTING)||test_is(SF_TEST_OVERHEAD));
        log_resources(t);
        take_birthdays();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(GUI_TICK_MS));
    }
}
