#include "skyframe_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool form(sf_config_t *c,const char *body){return sf_config_parse_form(c,body,strlen(body));}
/* The v2 record exactly as it shipped (one network), to check the upgrade path. */
typedef struct {
 char ssid[SF_WIFI_SSID_MAX+1];char password[SF_WIFI_PASS_MAX+1];
 double latitude,longitude;unsigned display_miles,activation_miles;
 unsigned radar_seconds,weather_seconds,photo_seconds;unsigned night_start_hour,night_end_hour;
} v2_t;
typedef struct {unsigned version;v2_t data;} v2_record_t;
/* The v5 record exactly as it shipped (before the display options). */
typedef struct {
 sf_wifi_network_t wifi[SF_WIFI_NETWORKS];
 double latitude,longitude;unsigned display_miles,activation_miles;
 unsigned radar_seconds,weather_seconds,photo_seconds;unsigned night_start_hour,night_end_hour;
 unsigned alert_flags;char alert_types[48];unsigned verse_seconds,today_seconds;
} v5_t;
typedef struct {unsigned version;v5_t data;} v5_record_t;
int main(void){
 sf_config_t c;sf_config_defaults(&c);assert(sf_config_valid(&c));
 assert(c.display_miles==10&&c.activation_miles==5&&sf_config_network_count(&c)==0);
 const char *good="ssid=Home+Network&password=verysecure123&lat=39.15&lon=-76.70&display=10&activation=5&radar=120&weather=30&photos=60";
 assert(sf_config_parse_form(&c,good,strlen(good)));assert(!strcmp(c.wifi[0].ssid,"Home Network"));
 sf_config_t original=c;
 assert(!sf_config_parse_form(&c,"activation=20&display=10",24));assert(!memcmp(&c,&original,sizeof(c)));
 assert(!sf_config_parse_form(&c,"ssid=Bad%00SSID",16));assert(!memcmp(&c,&original,sizeof(c)));
 assert(sf_config_parse_form(&c,"password=&weather=20",20));assert(!strcmp(c.wifi[0].password,"verysecure123"));
 assert(!sf_config_parse_form(&c,"lon=999",7));
 /* Night hours: defaults, accepted range 0-23, out-of-range rejected without changes. */
 assert(original.night_start_hour==SF_DEFAULT_NIGHT_START&&original.night_end_hour==SF_DEFAULT_NIGHT_END);
 assert(sf_config_parse_form(&c,"night_start=23&night_end=6",26)&&c.night_start_hour==23&&c.night_end_hour==6);
 original=c;
 assert(!sf_config_parse_form(&c,"night_start=24",14));assert(!memcmp(&c,&original,sizeof(c)));

 /* Several networks: add, update a password, prefer, remove, limit, no gaps. */
 assert(form(&c,"ssid=Cabin&password=cabinpass1")&&sf_config_network_count(&c)==2);
 assert(!strcmp(c.wifi[0].ssid,"Home Network")&&!strcmp(c.wifi[1].ssid,"Cabin"));
 assert(form(&c,"ssid=Cafe&password=")&&!c.wifi[2].password[0]);          /* open network */
 assert(form(&c,"ssid=Cabin&password=")&&!strcmp(c.wifi[1].password,"cabinpass1"));  /* kept */
 assert(form(&c,"ssid=Cabin&password=newcabin99")&&!strcmp(c.wifi[1].password,"newcabin99"));
 original=c;const char *why=NULL;
 assert(!sf_config_apply_form(&c,"ssid=Short&password=1234567",27,&why)&&why);assert(!memcmp(&c,&original,sizeof(c)));
 assert(form(&c,"prefer=Cafe")&&!strcmp(c.wifi[0].ssid,"Cafe")&&!strcmp(c.wifi[1].ssid,"Home Network")&&!strcmp(c.wifi[2].ssid,"Cabin"));
 assert(!strcmp(c.wifi[1].password,"verysecure123"));
 assert(form(&c,"remove=Home+Network")&&sf_config_network_count(&c)==2&&!strcmp(c.wifi[1].ssid,"Cabin"));
 assert(!c.wifi[2].ssid[0]&&!c.wifi[2].password[0]);
 assert(form(&c,"remove=Gone"));                                           /* stale page: no-op */
 assert(form(&c,"ssid=N3&password=password3")&&form(&c,"ssid=N4&password=password4")&&form(&c,"ssid=N5&password=password5"));
 assert(sf_config_network_count(&c)==5);original=c;why=NULL;
 assert(!sf_config_apply_form(&c,"ssid=N6&password=password6",26,&why)&&why);assert(!memcmp(&c,&original,sizeof(c)));
 assert(form(&c,"remove=N3&ssid=N6&password=password6")&&sf_config_network_count(&c)==5&&!strcmp(c.wifi[4].ssid,"N6"));
 assert(sf_config_find_network(&c,"N3")==-1&&sf_config_find_network(&c,"N6")==4);
 sf_config_t gap=c;gap.wifi[1].ssid[0]=0;gap.wifi[1].password[0]=0;assert(!sf_config_valid(&gap));
 sf_config_t dup=c;strcpy(dup.wifi[1].ssid,dup.wifi[0].ssid);assert(!sf_config_valid(&dup));

 /* Sighting alerts: defaults, the form's checkboxes, type lists. */
 sf_config_t d;sf_config_defaults(&d);
 assert(d.alert_flags==SF_DEFAULT_ALERTS&&!strcmp(d.alert_types,"B74*,A388"));
 assert(form(&d,"alerts=1&alert_vip=1&alert_types=b748%2C+a388+c17")&&d.alert_flags==(SF_ALERT_VIP|SF_ALERT_NO_OVERHEAD));
 assert(!strcmp(d.alert_types,"B748,A388,C17"));
 assert(form(&d,"radar=200")&&d.alert_flags==(SF_ALERT_VIP|SF_ALERT_NO_OVERHEAD));               /* no alert section: kept */
 assert(form(&d,"alerts=1&alert_overhead=1&alert_types=")&&d.alert_flags==0&&!d.alert_types[0]);
 original=d;why=NULL;
 assert(!sf_config_apply_form(&d,"alert_types=BOEING747",21,&why)&&why);assert(!memcmp(&d,&original,sizeof(d)));
 /* Verse and Today's sky screen times: 0 skips the page, otherwise 5-600 s. */
 assert(d.verse_seconds==SF_DEFAULT_VERSE_SECONDS&&d.today_seconds==SF_DEFAULT_TODAY_SECONDS);
 assert(form(&d,"verse=0&today=45")&&d.verse_seconds==0&&d.today_seconds==45);
 original=d;assert(!form(&d,"today=3"));assert(!memcmp(&d,&original,sizeof(d)));
 /* Page transitions: on unless the box is unticked on a form that shows it. */
 assert(!(d.ui_flags&SF_UI_NO_TRANSITIONS));
 assert(form(&d,"ui=1")&&(d.ui_flags&SF_UI_NO_TRANSITIONS));
 assert(form(&d,"radar=150")&&(d.ui_flags&SF_UI_NO_TRANSITIONS));
 assert(form(&d,"ui=1&transitions=1")&&!(d.ui_flags&SF_UI_NO_TRANSITIONS));
 char t[48];
 assert(sf_config_types_normalize(" b74* ,A388;;c5m ",t,sizeof(t))&&!strcmp(t,"B74*,A388,C5M"));
 assert(!sf_config_types_normalize("A",t,sizeof(t))&&!sf_config_types_normalize("****",t,sizeof(t)));
 assert(!sf_config_types_normalize("A1,A2,A3,A4,A5,A6,A7,A8,A9",t,sizeof(t)));

 /* A v5 device keeps everything, and gets the transitions on. */
 { v5_record_t r5;memset(&r5,0,sizeof(r5));r5.version=5;
   strcpy(r5.data.wifi[0].ssid,"Home");strcpy(r5.data.wifi[0].password,"password1");strcpy(r5.data.wifi[1].ssid,"Cabin");
   r5.data.latitude=39.1;r5.data.longitude=-76.7;r5.data.display_miles=12;r5.data.activation_miles=4;
   r5.data.radar_seconds=90;r5.data.weather_seconds=25;r5.data.photo_seconds=45;r5.data.night_start_hour=21;r5.data.night_end_hour=6;
   r5.data.alert_flags=SF_ALERT_VIP|SF_ALERT_NO_OVERHEAD;strcpy(r5.data.alert_types,"B748");
   r5.data.verse_seconds=33;r5.data.today_seconds=0;
   sf_config_t u;assert(sf_config_decode(&r5,sizeof(r5),&u));
   assert(!strcmp(u.wifi[0].ssid,"Home")&&!strcmp(u.wifi[0].password,"password1")&&!strcmp(u.wifi[1].ssid,"Cabin"));
   assert(u.latitude==39.1&&u.display_miles==12&&u.photo_seconds==45&&u.night_end_hour==6);
   assert(u.alert_flags==(SF_ALERT_VIP|SF_ALERT_NO_OVERHEAD)&&!strcmp(u.alert_types,"B748"));
   assert(u.verse_seconds==33&&u.today_seconds==0&&u.ui_flags==0); }

 /* Records: current round trip; a v2 device keeps its network and every setting. */
 sf_config_record_t rec;memset(&rec,0,sizeof(rec));rec.version=SF_CONFIG_RECORD_VERSION;rec.data=c;
 sf_config_t back;assert(sf_config_decode(&rec,sizeof(rec),&back)&&!memcmp(back.wifi,c.wifi,sizeof(c.wifi)));
 assert(back.latitude==c.latitude&&back.radar_seconds==c.radar_seconds&&back.night_end_hour==c.night_end_hour);
 assert(!sf_config_decode(&rec,sizeof(rec)-4,&back));
 v2_record_t old;memset(&old,0,sizeof(old));old.version=2;
 strcpy(old.data.ssid,"Old Home");strcpy(old.data.password,"oldpassword");
 old.data.latitude=40.5;old.data.longitude=-75.25;old.data.display_miles=20;old.data.activation_miles=8;
 old.data.radar_seconds=300;old.data.weather_seconds=45;old.data.photo_seconds=90;
 old.data.night_start_hour=21;old.data.night_end_hour=6;
 assert(sf_config_decode(&old,sizeof(old),&back));
 assert(sf_config_network_count(&back)==1&&!strcmp(back.wifi[0].ssid,"Old Home")&&!strcmp(back.wifi[0].password,"oldpassword"));
 assert(back.latitude==40.5&&back.longitude==-75.25&&back.display_miles==20&&back.activation_miles==8);
 assert(back.radar_seconds==300&&back.weather_seconds==45&&back.photo_seconds==90);
 assert(back.night_start_hour==21&&back.night_end_hour==6);
 old.data.ssid[0]=0;old.data.password[0]=0;   /* never set up */
 assert(sf_config_decode(&old,sizeof(old),&back)&&sf_config_network_count(&back)==0);
 old.version=7;assert(!sf_config_decode(&old,sizeof(old),&back));
 puts("config form tests: PASS");return 0;
}
