#include "skyframe_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <ctype.h>
static int dec(char c){if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool unescape(char *out,size_t size,const char *in,size_t len){
 size_t j=0;
 for(size_t i=0;i<len;i++){
  unsigned char c=(unsigned char)in[i];
  if(c=='%') {if(i+2>=len)return false;int a=dec(in[i+1]),b=dec(in[i+2]);if(a<0||b<0)return false;c=(unsigned char)(a*16+b);i+=2;}
  else if(c=='+')c=' ';
  if(!c||c<0x20||j+1>=size)return false;
  out[j++]=(char)c;
 }
 out[j]=0;return true;
}
static size_t bounded_len(const char *s,size_t cap){size_t n=0;while(n<cap&&s[n])n++;return n;}
const unsigned sf_radar_preset_miles[SF_RADAR_PRESETS]={1,5,10};
static const unsigned preset_activation[SF_RADAR_PRESETS]={1,3,5};
unsigned sf_radar_preset_activation(unsigned miles){
 for(int i=0;i<SF_RADAR_PRESETS;i++)if(sf_radar_preset_miles[i]==miles)return preset_activation[i];
 return 0;
}
bool sf_config_set_radar_preset(sf_config_t *cfg,unsigned miles){
 unsigned act=sf_radar_preset_activation(miles);
 if(!cfg||!act)return false;
 cfg->display_miles=miles;cfg->activation_miles=act;
 return true;
}
unsigned sf_radar_preset_next(unsigned miles){
 for(int i=0;i<SF_RADAR_PRESETS;i++)if(sf_radar_preset_miles[i]>miles)return sf_radar_preset_miles[i];
 return sf_radar_preset_miles[0];
}
unsigned sf_config_radar_preset(const sf_config_t *cfg){
 if(!cfg)return 0;
 unsigned act=sf_radar_preset_activation(cfg->display_miles);
 return act&&act==cfg->activation_miles?cfg->display_miles:0;
}
void sf_config_defaults(sf_config_t *cfg){
 memset(cfg,0,sizeof(*cfg));cfg->latitude=SF_DEFAULT_LAT;cfg->longitude=SF_DEFAULT_LON;
 cfg->display_miles=10;cfg->activation_miles=5;
 cfg->radar_seconds=120;cfg->weather_seconds=30;cfg->photo_seconds=60;
 cfg->night_start_hour=SF_DEFAULT_NIGHT_START;cfg->night_end_hour=SF_DEFAULT_NIGHT_END;
 cfg->alert_flags=SF_DEFAULT_ALERTS;
 cfg->verse_seconds=SF_DEFAULT_VERSE_SECONDS;cfg->today_seconds=SF_DEFAULT_TODAY_SECONDS;
 snprintf(cfg->alert_types,sizeof(cfg->alert_types),"%s",SF_DEFAULT_ALERT_TYPES);
}
bool sf_config_types_normalize(const char *in,char *out,size_t cap){
 char buf[sizeof(((sf_config_t*)0)->alert_types)];size_t n=0;int count=0;
 if(!in||!out||!cap)return false;
 for(const char *p=in;*p;){
  while(*p==','||*p==' '||*p==';')p++;
  if(!*p)break;
  const char *e=p;while(*e&&*e!=','&&*e!=' '&&*e!=';')e++;
  size_t k=(size_t)(e-p);
  bool star=p[k-1]=='*';
  size_t core=star?k-1:k;
  if(core<(star?1u:2u)||core>(star?3u:4u)||++count>8)return false;
  for(size_t i=0;i<core;i++)if(!isalnum((unsigned char)p[i]))return false;
  if(n+k+2>sizeof(buf))return false;
  if(n)buf[n++]=',';
  for(size_t i=0;i<k;i++)buf[n++]=(char)toupper((unsigned char)p[i]);
  p=e;
 }
 buf[n]=0;
 if(n+1>cap)return false;
 memcpy(out,buf,n+1);
 return true;
}
/* Wi-Fi list: used slots first, no duplicate names, WPA passwords 8+ characters. */
static bool wifi_valid(const sf_config_t *c){
 bool ended=false;
 for(int i=0;i<SF_WIFI_NETWORKS;i++){
  const sf_wifi_network_t *w=&c->wifi[i];
  size_t sl=bounded_len(w->ssid,sizeof(w->ssid)),pl=bounded_len(w->password,sizeof(w->password));
  if(sl>SF_WIFI_SSID_MAX||pl>SF_WIFI_PASS_MAX)return false;
  if(!sl){if(pl)return false;ended=true;continue;}
  if(ended||(pl&&pl<8))return false;
  for(int j=0;j<i;j++)if(!strcmp(c->wifi[j].ssid,w->ssid))return false;
 }
 return true;
}
bool sf_config_valid(const sf_config_t *c){
 if(!c||!isfinite(c->latitude)||!isfinite(c->longitude)||fabs(c->latitude)>90||fabs(c->longitude)>180)return false;
 if(c->display_miles<1||c->display_miles>100||c->activation_miles<1||c->activation_miles>c->display_miles)return false;
 if(c->radar_seconds<30||c->radar_seconds>3600||c->weather_seconds<5||c->weather_seconds>600||c->photo_seconds<10||c->photo_seconds>3600)return false;
 if(c->night_start_hour>23||c->night_end_hour>23)return false;
 if(c->alert_flags&~SF_ALERT_MASK)return false;
 if(c->ui_flags&~SF_UI_MASK)return false;
 if((c->verse_seconds&&(c->verse_seconds<5||c->verse_seconds>600))||(c->today_seconds&&(c->today_seconds<5||c->today_seconds>600)))return false;
 if(bounded_len(c->alert_types,sizeof(c->alert_types))>=sizeof(c->alert_types))return false;
 return wifi_valid(c);
}
int sf_config_network_count(const sf_config_t *c){
 int n=0;while(c&&n<SF_WIFI_NETWORKS&&c->wifi[n].ssid[0])n++;
 return n;
}
int sf_config_find_network(const sf_config_t *c,const char *ssid){
 if(!c||!ssid||!ssid[0])return -1;
 for(int i=0;i<SF_WIFI_NETWORKS&&c->wifi[i].ssid[0];i++)if(!strncmp(c->wifi[i].ssid,ssid,sizeof(c->wifi[i].ssid)))return i;
 return -1;
}
static void forget_network(sf_config_t *c,const char *ssid){
 int i=sf_config_find_network(c,ssid);
 if(i<0)return;                  /* already gone (e.g. an old page): nothing to do */
 memmove(&c->wifi[i],&c->wifi[i+1],(size_t)(SF_WIFI_NETWORKS-1-i)*sizeof(c->wifi[0]));
 memset(&c->wifi[SF_WIFI_NETWORKS-1],0,sizeof(c->wifi[0]));
}
static void prefer_network(sf_config_t *c,const char *ssid){
 int i=sf_config_find_network(c,ssid);
 if(i<=0)return;
 sf_wifi_network_t w=c->wifi[i];   /* the rest keep their order */
 memmove(&c->wifi[1],&c->wifi[0],(size_t)i*sizeof(c->wifi[0]));
 c->wifi[0]=w;
}
static bool number(const char *s,double *d){char *end;*d=strtod(s,&end);return *s&&*end==0&&isfinite(*d);}
static bool uintval(const char *s,unsigned *u){char *e;unsigned long v=strtoul(s,&e,10);if(!*s||*e||v>100000)return false;*u=(unsigned)v;return true;}
bool sf_config_apply_form(sf_config_t *cfg,const char *body,size_t len,const char **why){
 const char *unused;if(!why)why=&unused;
 *why=NULL;
 if(!cfg||!body||len>2048)return false;
 sf_config_t next=*cfg;
 char add_ssid[SF_WIFI_SSID_MAX+1]="",add_pass[SF_WIFI_PASS_MAX+1]="",drop[SF_WIFI_SSID_MAX+1]="",prefer[SF_WIFI_SSID_MAX+1]="";
 bool alert_form=false,overhead=false;unsigned alert_flags=0;
 bool ui_form=false,transitions=false;
 size_t p=0;
 while(p<len){size_t e=p;while(e<len&&body[e]!='&')e++;
  size_t eq=p;while(eq<e&&body[eq]!='=')eq++;if(eq==e)return false;
  char key[32],val[128];if(!unescape(key,sizeof(key),body+p,eq-p)||!unescape(val,sizeof(val),body+eq+1,e-eq-1))return false;
  double d;
  if(!strcmp(key,"ssid")){if(strlen(val)>SF_WIFI_SSID_MAX){*why="Wi-Fi names have at most 32 characters.";return false;}strcpy(add_ssid,val);}
  else if(!strcmp(key,"password")){if(strlen(val)>SF_WIFI_PASS_MAX){*why="Wi-Fi passwords have at most 64 characters.";return false;}strcpy(add_pass,val);}
  else if(!strcmp(key,"remove")){if(strlen(val)>SF_WIFI_SSID_MAX)return false;strcpy(drop,val);}
  else if(!strcmp(key,"prefer")){if(strlen(val)>SF_WIFI_SSID_MAX)return false;strcpy(prefer,val);}
  else if(!strcmp(key,"lat")){if(!number(val,&d))return false;next.latitude=d;}
  else if(!strcmp(key,"lon")){if(!number(val,&d))return false;next.longitude=d;}
  else if(!strcmp(key,"display")){if(!uintval(val,&next.display_miles))return false;}
  else if(!strcmp(key,"activation")){if(!uintval(val,&next.activation_miles))return false;}
  else if(!strcmp(key,"radar")){if(!uintval(val,&next.radar_seconds))return false;}
  else if(!strcmp(key,"weather")){if(!uintval(val,&next.weather_seconds))return false;}
  else if(!strcmp(key,"photos")){if(!uintval(val,&next.photo_seconds))return false;}
  else if(!strcmp(key,"night_start")){if(!uintval(val,&next.night_start_hour))return false;}
  else if(!strcmp(key,"night_end")){if(!uintval(val,&next.night_end_hour))return false;}
  else if(!strcmp(key,"verse")){if(!uintval(val,&next.verse_seconds))return false;}
  else if(!strcmp(key,"today")){if(!uintval(val,&next.today_seconds))return false;}
  else if(!strcmp(key,"ui"))ui_form=true;
  else if(!strcmp(key,"transitions"))transitions=true;
  else if(!strcmp(key,"alerts"))alert_form=true;
  else if(!strcmp(key,"alert_emergency"))alert_flags|=SF_ALERT_EMERGENCY;
  else if(!strcmp(key,"alert_vip"))alert_flags|=SF_ALERT_VIP;
  else if(!strcmp(key,"alert_military"))alert_flags|=SF_ALERT_MILITARY;
  else if(!strcmp(key,"alert_overhead"))overhead=true;
  else if(!strcmp(key,"alert_types")){
   if(!sf_config_types_normalize(val,next.alert_types,sizeof(next.alert_types))){
    *why="Aircraft types are codes of 2 to 4 letters or digits, like B748 or A388 (B74* means any 747), at most 8.";
    return false;
   }
  }
  else return false;
  p=e+1;
 }
 /* Unticked boxes aren't sent at all, so the section's marker says they were all shown. */
 if(alert_form)next.alert_flags=alert_flags|(overhead?0:SF_ALERT_NO_OVERHEAD);
 if(ui_form)next.ui_flags=transitions?next.ui_flags&~SF_UI_NO_TRANSITIONS:next.ui_flags|SF_UI_NO_TRANSITIONS;
 /* Forget first, so a full list can make room in the same save. */
 if(drop[0])forget_network(&next,drop);
 int count=sf_config_network_count(&next);
 if(add_ssid[0]){
  int i=sf_config_find_network(&next,add_ssid);
  if(i<0){
   if(count>=SF_WIFI_NETWORKS){*why="SkyFrame remembers up to 5 Wi-Fi networks. Remove one first.";return false;}
   i=count;strcpy(next.wifi[i].ssid,add_ssid);next.wifi[i].password[0]=0;
  }
  if(add_pass[0])strcpy(next.wifi[i].password,add_pass);   /* empty: keep the saved one */
 }else if(add_pass[0]){
  if(!count){*why="Please enter the Wi-Fi network name too.";return false;}
  strcpy(next.wifi[0].password,add_pass);
 }
 if(prefer[0])prefer_network(&next,prefer);
 if(!sf_config_valid(&next)){
  if(!wifi_valid(&next))*why="Wi-Fi passwords have at least 8 characters.";
  else if(next.activation_miles>next.display_miles)*why="\"Show the radar within\" can't be larger than the radar radius.";
  else *why="A value is outside the allowed range. Please check and try again.";
  return false;
 }
 *cfg=next;return true;
}
bool sf_config_parse_form(sf_config_t *cfg,const char *body,size_t len){return sf_config_apply_form(cfg,body,len,NULL);}

/* Earlier record layouts, exactly as they shipped (never change these). */
typedef struct {
    char ssid[SF_WIFI_SSID_MAX+1];
    char password[SF_WIFI_PASS_MAX+1];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
} config_v1_t;
typedef struct {unsigned version;config_v1_t data;} record_v1_t;
typedef struct {                       /* v2: v1 plus night hours */
    char ssid[SF_WIFI_SSID_MAX+1];
    char password[SF_WIFI_PASS_MAX+1];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
    unsigned night_start_hour, night_end_hour;
} config_v2_t;
typedef struct {unsigned version;config_v2_t data;} record_v2_t;
typedef struct {                       /* v3: several Wi-Fi networks */
    sf_wifi_network_t wifi[SF_WIFI_NETWORKS];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
    unsigned night_start_hour, night_end_hour;
} config_v3_t;
typedef struct {unsigned version;config_v3_t data;} record_v3_t;
typedef struct {                       /* v4: v3 plus sighting alerts */
    sf_wifi_network_t wifi[SF_WIFI_NETWORKS];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
    unsigned night_start_hour, night_end_hour;
    unsigned alert_flags;
    char alert_types[48];
} config_v4_t;
typedef struct {unsigned version;config_v4_t data;} record_v4_t;
typedef struct {                       /* v5: v4 plus verse / Today's sky screen times */
    sf_wifi_network_t wifi[SF_WIFI_NETWORKS];
    double latitude, longitude;
    unsigned display_miles, activation_miles;
    unsigned radar_seconds, weather_seconds, photo_seconds;
    unsigned night_start_hour, night_end_hour;
    unsigned alert_flags;
    char alert_types[48];
    unsigned verse_seconds, today_seconds;
} config_v5_t;
typedef struct {unsigned version;config_v5_t data;} record_v5_t;
/* v4's fields, which v5 also carries first. */
static void from_v4(sf_config_t *c,const config_v4_t *d){
 memcpy(c->wifi,d->wifi,sizeof(c->wifi));
 for(int i=0;i<SF_WIFI_NETWORKS;i++){c->wifi[i].ssid[SF_WIFI_SSID_MAX]=0;c->wifi[i].password[SF_WIFI_PASS_MAX]=0;}
 c->latitude=d->latitude;c->longitude=d->longitude;
 c->display_miles=d->display_miles;c->activation_miles=d->activation_miles;
 c->radar_seconds=d->radar_seconds;c->weather_seconds=d->weather_seconds;c->photo_seconds=d->photo_seconds;
 c->night_start_hour=d->night_start_hour;c->night_end_hour=d->night_end_hour;
 c->alert_flags=d->alert_flags;
 memcpy(c->alert_types,d->alert_types,sizeof(c->alert_types));
 c->alert_types[sizeof(c->alert_types)-1]=0;
}
/* The single network of v1/v2 becomes the first (preferred) saved network. */
static void single_network(sf_config_t *c,const char *ssid,const char *password){
 memcpy(c->wifi[0].ssid,ssid,SF_WIFI_SSID_MAX);c->wifi[0].ssid[SF_WIFI_SSID_MAX]=0;
 if(c->wifi[0].ssid[0]){memcpy(c->wifi[0].password,password,SF_WIFI_PASS_MAX);c->wifi[0].password[SF_WIFI_PASS_MAX]=0;}
}
bool sf_config_decode(const void *blob,size_t size,sf_config_t *out){
 unsigned version;
 if(!blob||!out||size<sizeof(version))return false;
 memcpy(&version,blob,sizeof(version));
 sf_config_t c;sf_config_defaults(&c);   /* fields an older record lacks keep their defaults */
 if(version==SF_CONFIG_RECORD_VERSION&&size==sizeof(sf_config_record_t)){
  memcpy(&c,(const char*)blob+offsetof(sf_config_record_t,data),sizeof(c));
  for(int i=0;i<SF_WIFI_NETWORKS;i++){c.wifi[i].ssid[SF_WIFI_SSID_MAX]=0;c.wifi[i].password[SF_WIFI_PASS_MAX]=0;}
  c.alert_types[sizeof(c.alert_types)-1]=0;
 }else if(version==5&&size==sizeof(record_v5_t)){
  record_v5_t r;memcpy(&r,blob,sizeof(r));
  config_v4_t v4;   /* v5 starts with v4's fields, in the same order */
  memcpy(v4.wifi,r.data.wifi,sizeof(v4.wifi));
  v4.latitude=r.data.latitude;v4.longitude=r.data.longitude;
  v4.display_miles=r.data.display_miles;v4.activation_miles=r.data.activation_miles;
  v4.radar_seconds=r.data.radar_seconds;v4.weather_seconds=r.data.weather_seconds;v4.photo_seconds=r.data.photo_seconds;
  v4.night_start_hour=r.data.night_start_hour;v4.night_end_hour=r.data.night_end_hour;
  v4.alert_flags=r.data.alert_flags;
  memcpy(v4.alert_types,r.data.alert_types,sizeof(v4.alert_types));
  from_v4(&c,&v4);
  c.verse_seconds=r.data.verse_seconds;c.today_seconds=r.data.today_seconds;
 }else if(version==4&&size==sizeof(record_v4_t)){
  record_v4_t r;memcpy(&r,blob,sizeof(r));
  from_v4(&c,&r.data);
 }else if(version==3&&size==sizeof(record_v3_t)){
  record_v3_t r;memcpy(&r,blob,sizeof(r));
  memcpy(c.wifi,r.data.wifi,sizeof(c.wifi));
  for(int i=0;i<SF_WIFI_NETWORKS;i++){c.wifi[i].ssid[SF_WIFI_SSID_MAX]=0;c.wifi[i].password[SF_WIFI_PASS_MAX]=0;}
  c.latitude=r.data.latitude;c.longitude=r.data.longitude;
  c.display_miles=r.data.display_miles;c.activation_miles=r.data.activation_miles;
  c.radar_seconds=r.data.radar_seconds;c.weather_seconds=r.data.weather_seconds;
  c.photo_seconds=r.data.photo_seconds;
  c.night_start_hour=r.data.night_start_hour;c.night_end_hour=r.data.night_end_hour;
 }else if(version==2&&size==sizeof(record_v2_t)){
  record_v2_t r;memcpy(&r,blob,sizeof(r));
  single_network(&c,r.data.ssid,r.data.password);
  c.latitude=r.data.latitude;c.longitude=r.data.longitude;
  c.display_miles=r.data.display_miles;c.activation_miles=r.data.activation_miles;
  c.radar_seconds=r.data.radar_seconds;c.weather_seconds=r.data.weather_seconds;
  c.photo_seconds=r.data.photo_seconds;
  c.night_start_hour=r.data.night_start_hour;c.night_end_hour=r.data.night_end_hour;
 }else if(version==1&&size==sizeof(record_v1_t)){
  record_v1_t r;memcpy(&r,blob,sizeof(r));
  single_network(&c,r.data.ssid,r.data.password);
  c.latitude=r.data.latitude;c.longitude=r.data.longitude;
  c.display_miles=r.data.display_miles;c.activation_miles=r.data.activation_miles;
  c.radar_seconds=r.data.radar_seconds;c.weather_seconds=r.data.weather_seconds;
  c.photo_seconds=r.data.photo_seconds;
 }else return false;
 if(!sf_config_valid(&c))return false;
 *out=c;return true;
}
