#include "skyframe_engine.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SF_PI 3.14159265358979323846
/* Tiny zero-copy JSON token walker. Structured validation is intentional:
   nested elements are skipped by matching their closing bracket. */
static const char *skip_ws(const char *p,const char *end){while(p<end&&isspace((unsigned char)*p))p++;return p;}
static const char *skip_string(const char *p,const char *e){
 if(p>=e||*p!='"')return NULL;
 p++;
 while(p<e){if(*p=='"')return p+1;if((unsigned char)*p<32)return NULL;
 if(*p=='\\'){p++;if(p>=e)return NULL;}p++;}return NULL;
}
static const char *skip_value(const char *p,const char *e,int depth){
 p=skip_ws(p,e);if(p>=e||depth>20)return NULL;
 if(*p=='"')return skip_string(p,e);
 if(*p=='{'||*p=='['){char close=(*p=='{')?'}':']';p=skip_ws(p+1,e);
 if(p<e&&*p==close)return p+1;
 while(p<e){if(close=='}'){
  p=skip_string(p,e);if(!p)return NULL;
 p=skip_ws(p,e);if(p>=e||*p++!=':')return NULL;
 }p=skip_value(p,e,depth+1);if(!p)return NULL;
 p=skip_ws(p,e);
 if(p<e&&*p==close)return p+1;
 if(p>=e||*p++!=',')return NULL;
 p=skip_ws(p,e);
 }return NULL;}
 if(*p=='-'||(*p>='0'&&*p<='9')){
  const char *start=p;if(*p=='-')p++;
  if(p>=e)return NULL;
  if(*p=='0')p++;else {if(*p<'1'||*p>'9')return NULL;while(p<e&&isdigit((unsigned char)*p))p++;}
  if(p<e&&*p=='.'){p++;if(p>=e||!isdigit((unsigned char)*p))return NULL;while(p<e&&isdigit((unsigned char)*p))p++;}
  if(p<e&&(*p=='e'||*p=='E')){p++;if(p<e&&(*p=='+'||*p=='-'))p++;if(p>=e||!isdigit((unsigned char)*p))return NULL;while(p<e&&isdigit((unsigned char)*p))p++;}
  return p>start?p:NULL;
 }
 const char *words[]={"true","false","null"};for(int i=0;i<3;i++){size_t n=strlen(words[i]);if((size_t)(e-p)>=n&&!memcmp(p,words[i],n))return p+n;}return NULL;
}
static bool field_name(const char *s,const char *e,const char *lit){size_t n=strlen(lit);return s<e&&*s=='"'&&(size_t)(e-s)==n+2&&!memcmp(s+1,lit,n);}
static bool field(const char *obj,const char *end,const char *name,const char **value,const char **value_end){
 if(obj>=end||*obj!='{')return false;
 const char *p=skip_ws(obj+1,end);
 while(p<end&&*p!='}'){
  const char *k=p;p=skip_string(p,end);if(!p)return false;const char *ke=p;
  p=skip_ws(p,end);if(p>=end||*p++!=':')return false;p=skip_ws(p,end);
  const char *v=p;p=skip_value(p,end,0);if(!p)return false;
  if(field_name(k,ke,name)){*value=v;*value_end=p;return true;}
  p=skip_ws(p,end);if(p<end&&*p==',')p=skip_ws(p+1,end);else break;
 }return false;
}
static bool num(const char *a,const char *b,double *out){
 if(!a||a>=b||*a=='"'||*a=='n'||*a=='t'||*a=='f')return false;
 char buf[64];size_t n=(size_t)(b-a);if(n>=sizeof(buf))return false;memcpy(buf,a,n);buf[n]=0;
 char *end=NULL;double v=strtod(buf,&end);if(end==buf||*end||!isfinite(v))return false;*out=v;return true;
}
static bool number_field(const char *a,const char *b,const char *name,double *v){const char *s=NULL,*e=NULL;return field(a,b,name,&s,&e)&&num(s,e,v);}
char sf_latin1_base(unsigned code){
 /* U+00C0..U+00FF: ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞß àáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ */
 static const char map[]="AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
 return (code>=0xC0&&code<=0xFF)?map[code-0xC0]:0;
}
void sf_ascii_fold(const char *in,char *out,size_t cap){
 if(!out||!cap)return;
 const unsigned char *p=(const unsigned char*)(in?in:"");
 size_t j=0;
 while(*p&&j+1<cap){
  if(*p<0x80){out[j++]=(char)*p++;continue;}
  if((p[0]&0xE0)==0xC0&&(p[1]&0xC0)==0x80){            /* 2-byte sequence */
   unsigned code=((p[0]&0x1Fu)<<6)|(p[1]&0x3Fu);
   char c=sf_latin1_base(code);
   if(!c&&code>=0x100&&code<=0x17F){                    /* Latin Extended-A: Ł ř ş ... */
    static const char ext[]="AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    c=code-0x100<sizeof(ext)-1?ext[code-0x100]:0;
   }
   if(code==0xA0)c=' ';
   if(c)out[j++]=c;
   p+=2;continue;
  }
  if(p[0]==0xE2&&p[1]==0x80&&p[2]){                     /* typographic punctuation */
   const char *rep=p[2]==0x98||p[2]==0x99?"'":p[2]==0x9C||p[2]==0x9D?"\"":p[2]==0x93||p[2]==0x94?"-":p[2]==0xA6?"...":"";
   for(;*rep&&j+1<cap;rep++)out[j++]=*rep;
   p+=3;continue;
  }
  p++;while((*p&0xC0)==0x80)p++;                       /* anything else: drop the character */
 }
 out[j]=0;
}

/* Copy a JSON string value, decoding escapes (\" \\ \/ \n...; \uXXXX outside ASCII
   becomes '?' since the display fonts are ASCII plus a few symbols). */
static void str_field(const char *a,const char *b,const char *name,char *dest,size_t cap){
 const char *s=NULL,*e=NULL;if(!field(a,b,name,&s,&e)||e-s<2||*s!='"')return;
 const char *p=s+1,*end=e-1;size_t n=0;
 while(p<end&&n+1<cap){
  char c=*p++;
  if(c=='\\'&&p<end){
   char x=*p++;
   if(x=='u'){unsigned code=0;int k=0;for(;k<4&&p<end&&isxdigit((unsigned char)*p);k++,p++)code=code*16+(unsigned)(isdigit((unsigned char)*p)?*p-'0':(tolower((unsigned char)*p)-'a'+10));
              c=(k==4&&code>=0x20&&code<0x7f)?(char)code:sf_latin1_base(code)?sf_latin1_base(code):'?';}
   else c=(x=='n'||x=='t'||x=='r'||x=='b'||x=='f')?' ':x;
  }
  dest[n++]=c;
 }
 dest[n]=0;
 sf_ascii_fold(dest,dest,cap);    /* raw UTF-8 in the JSON (not \u escaped) */
 n=strlen(dest);
 while(n>0&&isspace((unsigned char)dest[n-1]))dest[--n]=0;
}
void sf_geo(double a,double b,double c,double d,float *dist,float *bearing){
 const double deg=SF_PI/180.0,lat1=a*deg,lat2=c*deg,dlat=(c-a)*deg,dlon=(d-b)*deg;
 double h=sin(dlat/2)*sin(dlat/2)+cos(lat1)*cos(lat2)*sin(dlon/2)*sin(dlon/2);
 h=fmin(1.0,fmax(0.0,h));*dist=(float)(3958.7613*2.0*asin(sqrt(h)));
 double y=sin(dlon)*cos(lat2),x=cos(lat1)*sin(lat2)-sin(lat1)*cos(lat2)*cos(dlon);
 *bearing=(float)fmod(atan2(y,x)*180.0/SF_PI+360.0,360.0);
}
/* Airborne traffic first: near a big airport, parked and taxiing planes would otherwise
   take every slot from the planes actually flying over. */
static bool ranks_before(const sf_plane_t *a,const sf_plane_t *b){
 if(a->on_ground!=b->on_ground)return !a->on_ground;
 return a->distance_miles<b->distance_miles;
}
static void process(const char *start,const char *end,double hlat,double hlon,double radius,sf_aircraft_result_t *out){
 double lat,lon,seen=0;if(!number_field(start,end,"lat",&lat)||!number_field(start,end,"lon",&lon)||fabs(lat)>90||fabs(lon)>180){out->ignored++;return;}
 if(number_field(start,end,"seen_pos",&seen)&&seen>35){out->ignored++;return;}
 sf_plane_t p={0};p.latitude=lat;p.longitude=lon;p.position_age_s=(float)(seen>0?seen:0);
 sf_geo(hlat,hlon,lat,lon,&p.distance_miles,&p.bearing_deg);
 if(p.distance_miles>radius){out->ignored++;return;}
 str_field(start,end,"flight",p.callsign,sizeof(p.callsign));
 str_field(start,end,"hex",p.icao,sizeof(p.icao));
 if(!p.callsign[0])snprintf(p.callsign,sizeof(p.callsign),"%s",p.icao[0]?p.icao:"UNKNOWN");
 str_field(start,end,"desc",p.aircraft_type,sizeof(p.aircraft_type));
 if(!p.aircraft_type[0])str_field(start,end,"t",p.aircraft_type,sizeof(p.aircraft_type));
 if(!p.aircraft_type[0])strcpy(p.aircraft_type,"Unknown aircraft");
 double v=0;if(number_field(start,end,"alt_baro",&v)||number_field(start,end,"alt_geom",&v)){p.has_altitude=true;p.altitude_ft=(int)lround(v);}
 const char *alt=NULL,*altend=NULL;
 if(field(start,end,"alt_baro",&alt,&altend)&&altend-alt==8&&!memcmp(alt,"\"ground\"",8))p.on_ground=true;
 str_field(start,end,"r",p.registration,sizeof(p.registration));
 str_field(start,end,"t",p.type_code,sizeof(p.type_code));
 str_field(start,end,"ownOp",p.operator_name,sizeof(p.operator_name));
 str_field(start,end,"year",p.year,sizeof(p.year));
 str_field(start,end,"squawk",p.squawk,sizeof(p.squawk));
 str_field(start,end,"category",p.category,sizeof(p.category));
 str_field(start,end,"emergency",p.emergency,sizeof(p.emergency));
 if(number_field(start,end,"dbFlags",&v)&&v>=0&&v<256)p.db_flags=(unsigned)v;
 if(number_field(start,end,"baro_rate",&v)||number_field(start,end,"geom_rate",&v)){p.vert_rate_fpm=(int)lround(v);p.has_vert_rate=true;}
 if(number_field(start,end,"gs",&v)){p.speed_knots=(float)v;p.has_speed=true;}
 /* Pressure altitude dips below zero near the field on high-pressure days. Slow means
    taxiing or parked; a fast one is just above the runway (shown as "near ground"). */
 if(!p.on_ground&&p.has_altitude&&p.altitude_ft<0&&(!p.has_speed||p.speed_knots<60.f))p.on_ground=true;
 if(number_field(start,end,"track",&v)||number_field(start,end,"true_heading",&v)){
  p.heading_deg=(float)fmod(v+360.0,360.0);p.has_heading=true;
 }
 /* Count what sits on the ground at the airport (surface vehicles aside) for its label. */
 if(p.on_ground&&p.category[0]!='C'){
  float d,b;sf_geo(SF_BWI_LATITUDE,SF_BWI_LONGITUDE,lat,lon,&d,&b);
  if(d<=2.5f)out->airport_ground++;
 }
 /* Kept in order: airborne before ground, then nearest first. */
 size_t i=out->count;if(i==SF_MAX_PLANES){if(!ranks_before(&p,&out->planes[i-1]))return;i--;}
 else out->count++;
 while(i>0&&ranks_before(&p,&out->planes[i-1])){out->planes[i]=out->planes[i-1];i--;}
 out->planes[i]=p;
}
bool sf_parse_adsb(const char *json,size_t length,double lat,double lon,double radius,sf_aircraft_result_t *out){
 if(!out)return false;
 memset(out,0,sizeof(*out));
 if(!json||!length||length>SF_ADSB_MAX_BYTES||fabs(lat)>90||fabs(lon)>180||radius<=0)return false;
 const char *end=json+length,*start=skip_ws(json,end),*v=NULL,*ve=NULL;
 if(start>=end||*start!='{'||!field(start,end,"ac",&v,&ve)||*v!='[')return false;
 const char *p=skip_ws(v+1,ve);
 while(p<ve&&*p!=']'){
  const char *obj=p;p=skip_value(p,ve,0);if(!p||*obj!='{')return false;
  process(obj,p,lat,lon,radius,out);
  p=skip_ws(p,ve);if(*p==',')p=skip_ws(p+1,ve);else if(*p!=']')return false;
 }
 if(p>=ve||*p!=']')return false;
 /* Ensure the envelope is complete, not a truncated upstream response. */
 const char *full=skip_value(start,end,0);if(!full||skip_ws(full,end)!=end)return false;
 out->api_ok=true;return true;
}
bool sf_has_active_traffic(const sf_aircraft_result_t *r,float miles){
 if(!r||!r->api_ok)return false;
 for(size_t i=0;i<r->count;i++)if(!r->planes[i].on_ground&&r->planes[i].distance_miles<=miles)return true;
 return false;
}
void sf_schedule_init(sf_scheduler_t *s,uint32_t now){memset(s,0,sizeof(*s));s->screen=SF_SCREEN_RADAR;s->phase_start_ms=now;s->radar_ms=120000;s->weather_ms=30000;s->photos_ms=60000;}
sf_screen_t sf_schedule_tick(sf_scheduler_t *s,uint32_t now,bool ok,bool active,bool details){
 /* Clock rollovers work for any interval under 49 days. Caller should tick at least once a second. */
 static const uint32_t quiet_ms=30000;
 uint32_t elapsed=now-s->phase_start_ms;
 /* Details open: hold radar, and restart the quiet window so closing them doesn't
    immediately hand over to weather. */
 if(details&&s->screen==SF_SCREEN_RADAR){s->phase_start_ms=now;s->quiet_timing=false;return s->screen;}
 if(s->screen==SF_SCREEN_RADAR){
  /* Quiet time only counts while the feed is valid; an outage restarts the 30 s window. */
  if(ok){if(active){s->quiet_timing=false;}else if(!s->quiet_timing){s->quiet_timing=true;s->quiet_since_ms=now;}}
  else s->quiet_timing=false;
  if(elapsed>=s->radar_ms||(ok&&s->quiet_timing&&(uint32_t)(now-s->quiet_since_ms)>=quiet_ms)){
   s->screen=SF_SCREEN_WEATHER;s->phase_start_ms=now;s->quiet_timing=false;
  }
  return s->screen;
 }
 uint32_t length=s->screen==SF_SCREEN_WEATHER?s->weather_ms:s->screen==SF_SCREEN_PHOTOS?s->photos_ms:
                 s->screen==SF_SCREEN_VERSE?s->verse_ms:s->screen==SF_SCREEN_TODAY?s->today_ms:s->birthday_ms;
 /* An optional screen switched off while showing (e.g. birthday dismissed) ends at once. */
 if(elapsed>=length){
  s->screen=sf_schedule_next(s,s->screen);s->phase_start_ms=now;s->quiet_timing=false;
 }
 return s->screen;
}
/* ---- Birthdays ------------------------------------------------------------ */
static void trim(char *s){
 size_t n=strlen(s),i=0;
 while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;
 while(s[i]&&isspace((unsigned char)s[i]))i++;
 if(i)memmove(s,s+i,n-i+1);
}
static bool leap(int y){return (y%4==0&&y%100!=0)||y%400==0;}
static int days_in(int y,int m){static const int d[]={31,28,31,30,31,30,31,31,30,31,30,31};return m==2&&leap(y)?29:d[m-1];}
bool sf_birthday_parse_line(const char *line,sf_birthday_t *out){
 if(!line||!out)return false;
 memset(out,0,sizeof(*out));
 char buf[160];sf_ascii_fold(line,buf,sizeof(buf));   /* "José" -> "Jose" for the fonts */
 trim(buf);
 if(!buf[0]||buf[0]=='#')return false;
 char *fields[4]={0};int n=0;
 for(char *p=buf;n<4;){fields[n++]=p;char *c=strchr(p,',');if(!c)break;*c=0;p=c+1;}
 for(int i=0;i<n;i++)trim(fields[i]);
 if(n<2||!fields[0][0]||strlen(fields[0])>=sizeof(out->name))return false;
 int y=0,m=0,d=0;char extra;
 if(sscanf(fields[1],"%4d-%2d-%2d%c",&y,&m,&d,&extra)==3){if(y<1880||y>2200)return false;}
 else if(sscanf(fields[1],"%2d-%2d%c",&m,&d,&extra)==2)y=0;
 else return false;
 if(m<1||m>12||d<1||d>days_in(y?y:2000,m))return false;   /* 2000 is leap: allows Feb 29 */
 snprintf(out->name,sizeof(out->name),"%s",fields[0]);
 out->year=y;out->month=m;out->day=d;
 for(int i=2;i<n;i++){
  if(!strcasecmp(fields[i],"owner"))out->owner=true;
  else if(fields[i][0]&&!out->photo[0]&&strlen(fields[i])<sizeof(out->photo)&&!strchr(fields[i],'/')&&!strchr(fields[i],'\\'))
   snprintf(out->photo,sizeof(out->photo),"%s",fields[i]);
 }
 return true;
}
bool sf_birthday_parse_message(const char *line,char *out,size_t cap){
 if(!line||!out||!cap)return false;
 char buf[160];sf_ascii_fold(line,buf,sizeof(buf));
 trim(buf);
 if(strncasecmp(buf,"message:",8))return false;
 char *text=buf+8;
 trim(text);
 snprintf(out,cap,"%s",text);
 return true;
}
bool sf_birthday_is_today(const sf_birthday_t *b,int year,int month,int day){
 if(!b)return false;
 if(b->month==2&&b->day==29&&!leap(year))return month==2&&day==28;
 return b->month==month&&b->day==day;
}
int sf_birthday_age(const sf_birthday_t *b,int year){
 return (b&&b->year&&year>=b->year)?year-b->year:-1;
}
const char *sf_ordinal_suffix(int n){
 int t=n%100;
 if(t>=11&&t<=13)return "th";
 switch(n%10){case 1:return "st";case 2:return "nd";case 3:return "rd";default:return "th";}
}

sf_screen_t sf_schedule_next(const sf_scheduler_t *s,sf_screen_t current){
 switch(current){
 case SF_SCREEN_RADAR:   return SF_SCREEN_WEATHER;
 case SF_SCREEN_WEATHER: return s&&s->today_ms?SF_SCREEN_TODAY:s&&s->verse_ms?SF_SCREEN_VERSE:SF_SCREEN_PHOTOS;
 case SF_SCREEN_TODAY:   return s&&s->verse_ms?SF_SCREEN_VERSE:SF_SCREEN_PHOTOS;
 case SF_SCREEN_VERSE:   return SF_SCREEN_PHOTOS;
 case SF_SCREEN_PHOTOS:  return s&&s->birthday_ms?SF_SCREEN_BIRTHDAY:SF_SCREEN_RADAR;
 default:                return SF_SCREEN_RADAR;
 }
}
void sf_schedule_skip(sf_scheduler_t *s,uint32_t now){
 if(!s)return;
 s->screen=sf_schedule_next(s,s->screen);s->phase_start_ms=now;s->quiet_timing=false;
}
void sf_schedule_set_optional(sf_scheduler_t *s,unsigned verse_sec,unsigned birthday_sec){
 if(!s)return;
 s->verse_ms=verse_sec<=600?verse_sec*1000u:600000u;
 s->birthday_ms=birthday_sec<=600?birthday_sec*1000u:600000u;
}
void sf_schedule_set_today(sf_scheduler_t *s,unsigned sec){
 if(s)s->today_ms=sec<=600?sec*1000u:600000u;
}
void sf_schedule_hold(sf_scheduler_t *s,uint32_t now){
 if(s){s->phase_start_ms=now;s->quiet_timing=false;}
}
void sf_schedule_jump(sf_scheduler_t *s,sf_screen_t screen,uint32_t now){
 if(!s||screen>=SF_SCREEN_COUNT)return;
 s->screen=screen;s->phase_start_ms=now;s->quiet_timing=false;
}

void sf_schedule_set_durations(sf_scheduler_t *s,unsigned radar,unsigned weather,unsigned photos){
 if(!s)return;
 if(radar>=30&&radar<=3600)s->radar_ms=radar*1000u;
 if(weather>=5&&weather<=600)s->weather_ms=weather*1000u;
 if(photos>=10&&photos<=3600)s->photos_ms=photos*1000u;
}
