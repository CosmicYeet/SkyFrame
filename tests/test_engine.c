#include "skyframe_engine.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
static void near(float a,float b,float tol){assert(fabsf(a-b)<=tol);}
int main(void){
 FILE *f=fopen("tests/sample_adsb.json","rb");assert(f);char b[4096];size_t n=fread(b,1,sizeof(b),f);fclose(f);
 sf_aircraft_result_t r;
 assert(sf_parse_adsb(b,n,39.15,-76.70,10,&r));   /* a made-up point near BWI */
 assert(r.api_ok&&r.count==2);
 assert(!strcmp(r.planes[0].callsign,"UAL747"));
 near(r.planes[0].distance_miles,6.79f,.12f);
 near(r.planes[0].heading_deg,310.09f,.01f);
 assert(r.planes[0].has_altitude&&r.planes[0].altitude_ft==1775);
 assert(!strcmp(r.planes[1].callsign,"UPS1207"));
 assert(r.planes[1].on_ground&&!r.planes[1].has_altitude);
 near(r.planes[1].heading_deg,165.94f,.01f);
 assert(sf_has_active_traffic(&r,8));
 { sf_aircraft_result_t g=r; g.planes[0]=g.planes[1]; g.count=1;   /* only a plane on the ground */
   assert(!sf_has_active_traffic(&g,10)); }
 assert(sf_parse_adsb("{\"ac\":[]}",9,39,-76,10,&r)&&r.count==0);
 assert(!sf_has_active_traffic(&r,5));
 assert(!sf_parse_adsb("{\"ac\":[",7,39,-76,10,&r));
 assert(!r.api_ok);
 const char *stale="{\"ac\":[{\"lat\":39.1,\"lon\":-76.6,\"seen_pos\":99}]}";
 assert(sf_parse_adsb(stale,strlen(stale),39.1,-76.6,10,&r)&&r.count==0);
 float airport_distance, airport_bearing;
 sf_geo(39.15,-76.70,SF_BWI_LATITUDE,SF_BWI_LONGITUDE,
        &airport_distance,&airport_bearing);
 near(airport_distance,2.62335f,0.005f);
 near(airport_bearing,38.30780f,0.05f);
 assert(airport_distance < SF_DISPLAY_MILES);
 assert(airport_distance < SF_ACTIVATION_MILES); /* marker itself does not trigger scheduler */
 sf_scheduler_t s;sf_schedule_init(&s,0);
 assert(sf_schedule_tick(&s,60000,true,true,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,120000,true,true,false)==SF_SCREEN_WEATHER);
 assert(sf_schedule_tick(&s,150000,true,true,false)==SF_SCREEN_PHOTOS);
 assert(sf_schedule_tick(&s,210000,true,true,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,212000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,242000,true,false,false)==SF_SCREEN_WEATHER);
 sf_schedule_init(&s,0);
 assert(sf_schedule_tick(&s,30000,false,false,false)==SF_SCREEN_RADAR); /* outage != clear sky */
 assert(sf_schedule_tick(&s,60000,true,true,true)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,61000,true,true,false)==SF_SCREEN_RADAR);
 /* Negative pressure altitude: slow means on the ground, fast stays airborne. */
 const char *low="{\"ac\":[{\"hex\":\"a1\",\"lat\":39.1,\"lon\":-76.6,\"alt_baro\":-75,\"gs\":12},"
                 "{\"hex\":\"a2\",\"lat\":39.1,\"lon\":-76.6,\"alt_baro\":-75,\"gs\":140}]}";
 assert(sf_parse_adsb(low,strlen(low),39.1,-76.6,10,&r)&&r.count==2);
 assert(r.planes[0].on_ground!=r.planes[1].on_ground);
 /* Escaped JSON strings are decoded; non-ASCII becomes '?'. */
 const char *esc="{\"ac\":[{\"hex\":\"a3\",\"lat\":39.1,\"lon\":-76.6,\"flight\":\"AB\\\"C\","
                 "\"ownOp\":\"X\\u0041Y\\u00e9\"}]}";
 assert(sf_parse_adsb(esc,strlen(esc),39.1,-76.6,10,&r)&&r.count==1);
 assert(!strcmp(r.planes[0].callsign,"AB\"C")&&!strcmp(r.planes[0].operator_name,"XAY?"));
 /* Closing a plane's details restarts the quiet window (no instant jump to weather). */
 sf_schedule_init(&s,0);
 assert(sf_schedule_tick(&s,1000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,20000,true,false,true)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,40000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,69000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,70000,true,false,false)==SF_SCREEN_WEATHER);
 /* Optional screens: verse after weather, birthday after photos; 0 skips them. */
 sf_schedule_init(&s,0);
 sf_schedule_set_optional(&s,20,15);
 assert(sf_schedule_tick(&s,120000,true,true,false)==SF_SCREEN_WEATHER);
 assert(sf_schedule_tick(&s,150000,true,true,false)==SF_SCREEN_VERSE);
 assert(sf_schedule_tick(&s,170000,true,true,false)==SF_SCREEN_PHOTOS);
 assert(sf_schedule_tick(&s,230000,true,true,false)==SF_SCREEN_BIRTHDAY);
 sf_schedule_set_optional(&s,20,0);   /* birthday dismissed while showing: leave at once */
 assert(sf_schedule_tick(&s,231000,true,true,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_next(&s,SF_SCREEN_PHOTOS)==SF_SCREEN_RADAR);
 /* ASCII folding for the display fonts. */
 char folded[32];
 sf_ascii_fold("Z\xC3\xBCrich S\xC3\xA3o Jos\xC3\xA9",folded,sizeof(folded));assert(!strcmp(folded,"Zurich Sao Jose"));
 sf_ascii_fold("\xE2\x80\x9CHi\xE2\x80\x9D \xE2\x80\x94 \xE2\x9C\x88ok",folded,sizeof(folded));assert(!strcmp(folded,"\"Hi\" - ok"));
 /* Birthday file lines. */
 sf_birthday_t bd;
 assert(sf_birthday_parse_line("  Alex, 1960-01-15, alex.jpg, owner ",&bd));
 assert(!strcmp(bd.name,"Alex")&&bd.year==1960&&bd.month==1&&bd.day==15&&!strcmp(bd.photo,"alex.jpg")&&bd.owner);
 assert(sf_birthday_age(&bd,2026)==66&&!strcmp(sf_ordinal_suffix(66),"th"));
 assert(sf_birthday_is_today(&bd,2026,1,15)&&!sf_birthday_is_today(&bd,2026,1,16));
 assert(sf_birthday_parse_line("Sam, 03-02",&bd)&&bd.year==0&&sf_birthday_age(&bd,2026)==-1&&!bd.owner&&!bd.photo[0]);
 assert(sf_birthday_parse_line("Leap, 2004-02-29",&bd)&&sf_birthday_is_today(&bd,2026,2,28)&&sf_birthday_is_today(&bd,2028,2,29));
 assert(!sf_birthday_parse_line("# comment",&bd)&&!sf_birthday_parse_line("",&bd));
 char msg[64];
 assert(sf_birthday_parse_message("  Message:  We love you, Alex! ",msg,sizeof(msg))&&!strcmp(msg,"We love you, Alex!"));
 assert(!sf_birthday_parse_message("Alex, 1960-01-15",msg,sizeof(msg))&&!sf_birthday_parse_line("message: hi",&bd));
 assert(!sf_birthday_parse_line("Bad, 2026-02-30",&bd)&&!sf_birthday_parse_line("Bad, 1999-13-01",&bd));
 assert(!sf_birthday_parse_line("Path, 1990-01-01, ../x.jpg",&bd)||!bd.photo[0]);
 assert(!strcmp(sf_ordinal_suffix(1),"st")&&!strcmp(sf_ordinal_suffix(22),"nd")&&!strcmp(sf_ordinal_suffix(13),"th")&&!strcmp(sf_ordinal_suffix(111),"th"));
 /* Quiet window restarts after an outage instead of counting time with no data. */
 sf_schedule_init(&s,0);
 assert(sf_schedule_tick(&s,1000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,20000,false,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,32000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,61000,true,false,false)==SF_SCREEN_RADAR);
 assert(sf_schedule_tick(&s,62000,true,false,false)==SF_SCREEN_WEATHER);
 puts("PASS: ADS-B parsing, BWI landmark position, heading, filtering, scheduler");return 0;
}