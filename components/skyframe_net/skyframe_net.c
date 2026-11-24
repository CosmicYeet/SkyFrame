#include "skyframe_net.h"
#include "skyframe_urls.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>

static const char *TAG="skyframe_net";
typedef struct {char *body;size_t used,cap;bool overflow;} reply_t;
static esp_err_t on_event(esp_http_client_event_t *e){
 reply_t *r=(reply_t*)e->user_data;
 /* Automatic redirects deliver every response's body here, so keep only the last
    response: start over at each new set of headers and skip 3xx bodies. */
 if(e->event_id==HTTP_EVENT_ON_HEADERS_COMPLETE&&r&&r->body){r->used=0;r->overflow=false;r->body[0]=0;return ESP_OK;}
 if(e->event_id!=HTTP_EVENT_ON_DATA)return ESP_OK;
 int status=esp_http_client_get_status_code(e->client);
 if(status>=300&&status<400)return ESP_OK;
 if(!r||e->data_len<0)return ESP_FAIL;
 size_t bytes=(size_t)e->data_len;
 if(bytes>r->cap-r->used-1){r->overflow=true;return ESP_FAIL;}
 memcpy(r->body+r->used,e->data,bytes);r->used+=bytes;r->body[r->used]=0;
 return ESP_OK;
}
/* One TLS session at a time: concurrent handshakes (traffic + route lookup) exhausted RAM. */
static portMUX_TYPE fetch_lock_init=portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t fetch_lock_buf;
static SemaphoreHandle_t fetch_lock;
static esp_err_t fetch_locked(const char *url,size_t max_payload,reply_t *reply,bool quiet_404);
static esp_err_t fetch_ex(const char *url,size_t max_payload,reply_t *reply,bool quiet_404){
 memset(reply,0,sizeof(*reply));
 taskENTER_CRITICAL(&fetch_lock_init);
 if(!fetch_lock)fetch_lock=xSemaphoreCreateMutexStatic(&fetch_lock_buf);
 taskEXIT_CRITICAL(&fetch_lock_init);
 if(xSemaphoreTake(fetch_lock,pdMS_TO_TICKS(30000))!=pdTRUE)return ESP_ERR_TIMEOUT;
 esp_err_t rc=fetch_locked(url,max_payload,reply,quiet_404);
 xSemaphoreGive(fetch_lock);
 return rc;
}
static esp_err_t fetch(const char *url,size_t max_payload,reply_t *reply){return fetch_ex(url,max_payload,reply,false);}
/* cJSON makes hundreds of small allocations per weather reply. Below 16 KB, malloc() uses
   scarce internal RAM first (needed by Wi-Fi and TLS), so give cJSON PSRAM instead. */
static void *json_alloc(size_t n){return heap_caps_malloc_prefer(n,2,MALLOC_CAP_SPIRAM,MALLOC_CAP_DEFAULT);}
static cJSON *json_parse(const char *text,size_t len){
 static bool hooked;
 if(!hooked){cJSON_Hooks h={.malloc_fn=json_alloc,.free_fn=free};cJSON_InitHooks(&h);hooked=true;}
 return cJSON_ParseWithLength(text,len);
}
static esp_err_t fetch_locked(const char *url,size_t max_payload,reply_t *reply,bool quiet_404){
 /* Allocate payload buffer from PSRAM when possible; do not consume all internal SRAM. */
 reply->body=heap_caps_malloc(max_payload+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!reply->body)return ESP_ERR_NO_MEM;
 reply->cap=max_payload+1;reply->body[0]=0;
 esp_http_client_config_t cfg={.url=url,.timeout_ms=12000,.event_handler=on_event,
     .user_data=reply,.crt_bundle_attach=esp_crt_bundle_attach,.buffer_size=2048,
     .user_agent="SkyFrame/0.4 (personal noncommercial radar)"};
 esp_http_client_handle_t client=esp_http_client_init(&cfg);
 if(!client){free(reply->body);reply->body=NULL;return ESP_FAIL;}
 esp_err_t rc=esp_http_client_perform(client);
 int status=esp_http_client_get_status_code(client);
 esp_http_client_cleanup(client);
 if(rc==ESP_OK&&status==404)rc=ESP_ERR_NOT_FOUND;
 else if(rc==ESP_OK&&(status!=200||reply->overflow||reply->used==0))rc=ESP_FAIL;
 if(rc!=ESP_OK){
  if(!(quiet_404&&rc==ESP_ERR_NOT_FOUND))
   ESP_LOGW(TAG,"HTTPS fetch failed: status=%d err=%s oversized=%d",status,esp_err_to_name(rc),reply->overflow);
  free(reply->body);reply->body=NULL;
 }
 return rc;
}
esp_err_t sf_net_get_aircraft(double lat,double lon,float radius,sf_aircraft_result_t *out){
 if(!out)return ESP_ERR_INVALID_ARG;
 memset(out,0,sizeof(*out));
 char url[180];if(!sf_aircraft_url(url,sizeof(url),lat,lon,radius))return ESP_ERR_INVALID_ARG;
 reply_t r;esp_err_t rc=fetch(url,SF_NET_AIRCRAFT_LIMIT,&r);if(rc!=ESP_OK)return rc;
 bool ok=sf_parse_adsb(r.body,r.used,lat,lon,radius,out);
 free(r.body);
 return ok?ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
static bool json_num(cJSON *parent,const char *key,double *out){
 cJSON *v=cJSON_GetObjectItemCaseSensitive(parent,key);
 if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble))return false;
 *out=v->valuedouble;return true;
}
static void json_str(cJSON *parent,const char *key,char *dest,size_t cap){
 cJSON *v=cJSON_GetObjectItemCaseSensitive(parent,key);
 if(cJSON_IsString(v)&&v->valuestring)sf_ascii_fold(v->valuestring,dest,cap);   /* "Zürich" -> "Zurich" */
}
/* "2026-11-19T16:52" -> "16:52" */
static void json_clock(cJSON *item,char *dest,size_t cap){
 if(!cJSON_IsString(item)||!item->valuestring)return;
 const char *t=strchr(item->valuestring,'T');
 snprintf(dest,cap,"%.5s",t?t+1:item->valuestring);
}
esp_err_t sf_net_get_weather(double lat,double lon,sf_weather_t *out){
 if(!out)return ESP_ERR_INVALID_ARG;
 memset(out,0,sizeof(*out));
 char url[512];if(!sf_weather_url(url,sizeof(url),lat,lon))return ESP_ERR_INVALID_ARG;
 reply_t r;esp_err_t rc=fetch(url,SF_NET_WEATHER_LIMIT,&r);if(rc!=ESP_OK)return rc;
 cJSON *root=json_parse(r.body,r.used);free(r.body);
 if(!root)return ESP_ERR_INVALID_RESPONSE;
 cJSON *current=cJSON_GetObjectItemCaseSensitive(root,"current");
 cJSON *daily=cJSON_GetObjectItemCaseSensitive(root,"daily");
 double temp=0,code=0,day=0,v=0;
 bool valid=cJSON_IsObject(current)&&cJSON_IsObject(daily)&&
   json_num(current,"temperature_2m",&temp)&&json_num(current,"weather_code",&code)&&
   json_num(current,"is_day",&day);
 cJSON *highs=cJSON_GetObjectItemCaseSensitive(daily,"temperature_2m_max");
 cJSON *lows=cJSON_GetObjectItemCaseSensitive(daily,"temperature_2m_min");
 cJSON *high=cJSON_GetArrayItem(highs,0),*low=cJSON_GetArrayItem(lows,0);
 valid=valid&&cJSON_IsNumber(high)&&cJSON_IsNumber(low)&&isfinite(high->valuedouble)&&isfinite(low->valuedouble);
 if(valid){
   out->temp_f=(float)temp;out->weather_code=(int)code;out->is_day=(day!=0);
   out->high_f=(float)high->valuedouble;out->low_f=(float)low->valuedouble;
   /* Supporting details are optional; a missing field shows as "--", never a made-up value. */
   out->feels_f=json_num(current,"apparent_temperature",&v)?(float)v:NAN;
   out->humidity=json_num(current,"relative_humidity_2m",&v)?(int)lround(v):-1;
   out->wind_mph=json_num(current,"wind_speed_10m",&v)?(float)v:NAN;
   out->wind_dir_deg=json_num(current,"wind_direction_10m",&v)?(float)v:NAN;
   cJSON *rain=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily,"precipitation_probability_max"),0);
   out->precip_chance=(cJSON_IsNumber(rain)&&isfinite(rain->valuedouble))?(int)lround(rain->valuedouble):-1;
   if(json_num(root,"utc_offset_seconds",&v)&&fabs(v)<=14*3600){out->utc_offset_s=(int)v;out->has_utc_offset=true;}
   json_clock(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily,"sunrise"),0),out->sunrise,sizeof(out->sunrise));
   json_clock(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily,"sunset"),0),out->sunset,sizeof(out->sunset));
   cJSON *time=cJSON_GetObjectItemCaseSensitive(current,"time");
   if(cJSON_IsString(time)&&time->valuestring)snprintf(out->observation_time,sizeof(out->observation_time),"%s",time->valuestring);
   /* Next SF_WEATHER_HOURS forecast slots; do not fail current weather if absent. */
   cJSON *hourly=cJSON_GetObjectItemCaseSensitive(root,"hourly");
   cJSON *times=cJSON_GetObjectItemCaseSensitive(hourly,"time");
   cJSON *temps=cJSON_GetObjectItemCaseSensitive(hourly,"temperature_2m");
   cJSON *codes=cJSON_GetObjectItemCaseSensitive(hourly,"weather_code");
   cJSON *days=cJSON_GetObjectItemCaseSensitive(hourly,"is_day");
   cJSON *chances=cJSON_GetObjectItemCaseSensitive(hourly,"precipitation_probability");
   int picked=0;
   for(int i=0; i<cJSON_GetArraySize(times) && picked<SF_WEATHER_HOURS; i++){
      cJSON *t=cJSON_GetArrayItem(times,i), *tv=cJSON_GetArrayItem(temps,i), *w=cJSON_GetArrayItem(codes,i);
      const char *current_s=(cJSON_IsString(time)?time->valuestring:NULL);
      if(current_s && cJSON_IsString(t) && t->valuestring &&
         strncmp(t->valuestring,current_s,13)<=0) continue;
      if(cJSON_IsString(t)&&t->valuestring&&cJSON_IsNumber(tv)&&cJSON_IsNumber(w)){
         const char *hh=strchr(t->valuestring,'T');
         snprintf(out->hourly[picked].time,sizeof(out->hourly[picked].time),"%s",hh?hh+1:t->valuestring);
         out->hourly[picked].temp_f=(float)tv->valuedouble;
         out->hourly[picked].weather_code=w->valueint;
         cJSON *d=cJSON_GetArrayItem(days,i);
         out->hourly[picked].is_day=cJSON_IsNumber(d)?(d->valueint!=0):true;
         cJSON *pc=cJSON_GetArrayItem(chances,i);
         out->hourly[picked].precip_chance=cJSON_IsNumber(pc)?pc->valueint:-1;
         out->hourly[picked].valid=true; picked++;
      }
   }
   out->valid=true;
 }
 cJSON_Delete(root);
 return valid?ESP_OK:ESP_ERR_INVALID_RESPONSE;
}

static void airport(cJSON *a,sf_airport_t *out){
 if(!cJSON_IsObject(a))return;
 json_str(a,"iata_code",out->code,sizeof(out->code));
 if(strlen(out->code)!=3)json_str(a,"icao_code",out->code,sizeof(out->code));
 json_str(a,"name",out->name,sizeof(out->name));
 json_str(a,"municipality",out->city,sizeof(out->city));
}
static bool alnum_only(const char *s,size_t min,size_t max){
 size_t n=s?strlen(s):0;if(n<min||n>max)return false;
 for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i]))return false;
 return true;
}
esp_err_t sf_net_get_aircraft_info(const char *icao,const char *callsign,sf_aircraft_info_t *info){
 if(!info||!alnum_only(icao,6,6))return ESP_ERR_INVALID_ARG;
 memset(info,0,sizeof(*info));
 snprintf(info->icao,sizeof(info->icao),"%s",icao);
 char url[120];
 /* Route is looked up in the same call when the callsign is usable. */
 if(alnum_only(callsign,3,8))snprintf(url,sizeof(url),"https://api.adsbdb.com/v0/aircraft/%s?callsign=%s",icao,callsign);
 else snprintf(url,sizeof(url),"https://api.adsbdb.com/v0/aircraft/%s",icao);
 reply_t reply;esp_err_t e=fetch_ex(url,16384,&reply,true);
 if(e==ESP_ERR_NOT_FOUND){info->lookup_ok=true;return ESP_OK;}   /* unknown airframe, not a failure */
 if(e!=ESP_OK)return e;
 cJSON *root=json_parse(reply.body,reply.used);free(reply.body);
 if(!root)return ESP_ERR_INVALID_RESPONSE;
 info->lookup_ok=true;
 cJSON *r=cJSON_GetObjectItemCaseSensitive(root,"response");
 cJSON *a=cJSON_GetObjectItemCaseSensitive(r,"aircraft");
 if(cJSON_IsObject(a)){
  info->aircraft_found=true;
  json_str(a,"manufacturer",info->manufacturer,sizeof(info->manufacturer));
  json_str(a,"type",info->model,sizeof(info->model));
  json_str(a,"icao_type",info->type_code,sizeof(info->type_code));
  json_str(a,"registration",info->registration,sizeof(info->registration));
  json_str(a,"registered_owner",info->owner,sizeof(info->owner));
  json_str(a,"registered_owner_country_name",info->owner_country,sizeof(info->owner_country));
  json_str(a,"url_photo",info->photo_url,sizeof(info->photo_url));
  if(strncmp(info->photo_url,"https://",8))info->photo_url[0]=0;   /* HTTPS only */
 }
 cJSON *fr=cJSON_GetObjectItemCaseSensitive(r,"flightroute");
 if(cJSON_IsObject(fr)){
  json_str(cJSON_GetObjectItemCaseSensitive(fr,"airline"),"name",info->airline,sizeof(info->airline));
  airport(cJSON_GetObjectItemCaseSensitive(fr,"origin"),&info->origin);
  airport(cJSON_GetObjectItemCaseSensitive(fr,"destination"),&info->destination);
  info->route_found=info->origin.code[0]&&info->destination.code[0];
 }
 cJSON_Delete(root);return ESP_OK;
}

/* Append UTF-8 text as ASCII: curly quotes -> straight, dashes -> '-', ellipsis -> "...",
   other non-ASCII dropped; whitespace runs collapse to one space. */
static void append_ascii(char *dst,size_t cap,const char *src){
 size_t n=strlen(dst);
 const unsigned char *p=(const unsigned char*)src;
 while(*p&&n+4<cap){
  const char *rep=NULL;int skip=1;
  if(p[0]==0xE2&&p[1]==0x80&&p[2]){
   skip=3;
   switch(p[2]){case 0x98:case 0x99:rep="'";break;case 0x9C:case 0x9D:rep="\"";break;
                case 0x93:case 0x94:rep="-";break;case 0xA6:rep="...";break;default:rep="";break;}
  }else if(*p>=0x80){skip=1;while(p[skip]>=0x80&&p[skip]<0xC0)skip++;rep="";}
  else if(isspace(*p)){rep=(n&&dst[n-1]!=' ')?" ":"";}
  if(rep){size_t k=strlen(rep);if(n+k+1<cap){memcpy(dst+n,rep,k);n+=k;}}
  else dst[n++]=(char)*p;
  p+=skip;
 }
 while(n&&dst[n-1]==' ')n--;
 dst[n]=0;
}
/* ---- Verse of the day (BibleGateway) -----------------------------------------
   https://www.biblegateway.com/votd/get/?format=json&version=NKJV returns the day's
   verse in the requested translation. BibleGateway requires the "Powered by
   BibleGateway.com" credit to stay visible. */
void sf_verse_decode_entities(const char *in,char *out,size_t cap){
 static const struct {const char *name,*text;} named[]={
  {"amp","&"},{"quot","\""},{"apos","'"},{"lt","<"},{"gt",">"},{"nbsp"," "},
  {"ldquo","\""},{"rdquo","\""},{"lsquo","'"},{"rsquo","'"},{"mdash","-"},{"ndash","-"},{"hellip","..."}};
 if(!out||!cap)return;
 size_t j=0;
 for(const char *p=in?in:"";*p&&j+1<cap;){
  if(*p=='&'){
   const char *semi=strchr(p,';');
   if(semi&&semi-p<=8){
    char name[9];size_t n=(size_t)(semi-p-1);memcpy(name,p+1,n);name[n]=0;
    const char *rep=NULL;char num[2]={0};
    if(name[0]=='#'){
     long v=name[1]=='x'||name[1]=='X'?strtol(name+2,NULL,16):strtol(name+1,NULL,10);
     if(v==0x2018||v==0x2019)rep="'";else if(v==0x201C||v==0x201D)rep="\"";
     else if(v==0x2013||v==0x2014)rep="-";else if(v==0x2026)rep="...";
     else if(v==0xA0||(v>=0x2000&&v<=0x200A))rep=" ";   /* non-breaking / thin spaces */
     else if(v>=0x20&&v<0x7F){num[0]=(char)v;rep=num;}else rep="";
    }else for(size_t i=0;i<sizeof(named)/sizeof(named[0]);i++)if(!strcmp(name,named[i].name)){rep=named[i].text;break;}
    if(rep){for(const char *r=rep;*r&&j+1<cap;r++)out[j++]=*r;p=semi+1;continue;}
   }
  }
  out[j++]=*p++;
 }
 out[j]=0;
}
static esp_err_t verse_biblegateway(const char *version,sf_verse_t *out){
 char url[96];
 snprintf(url,sizeof(url),"https://www.biblegateway.com/votd/get/?format=json&version=%s",version);
 reply_t r;esp_err_t e=fetch(url,16384,&r);
 if(e!=ESP_OK)return e;
 cJSON *root=json_parse(r.body,r.used);free(r.body);
 cJSON *v=cJSON_GetObjectItemCaseSensitive(root,"votd");
 cJSON *text=cJSON_GetObjectItemCaseSensitive(v,"rawtext"),*ref=cJSON_GetObjectItemCaseSensitive(v,"display_ref");
 cJSON *vid=cJSON_GetObjectItemCaseSensitive(v,"version_id");
 /* Only accept the translation asked for, never a substitute. */
 if(cJSON_IsString(text)&&cJSON_IsString(ref)&&cJSON_IsString(vid)&&!strcasecmp(vid->valuestring,version)){
  char *plain=malloc(sizeof(out->text));
  if(plain){
   sf_verse_decode_entities(text->valuestring,plain,sizeof(out->text));
   append_ascii(out->text,sizeof(out->text),plain);
   sf_verse_decode_entities(ref->valuestring,plain,sizeof(out->reference));
   append_ascii(out->reference,sizeof(out->reference),plain);
   free(plain);
  }
  snprintf(out->version,sizeof(out->version),"%s",version);
  snprintf(out->credit,sizeof(out->credit),"Powered by BibleGateway.com");
  out->valid=out->text[0]&&out->reference[0];
 }
 cJSON_Delete(root);
 return out->valid?ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
esp_err_t sf_net_get_verse(sf_verse_t *out){
 if(!out)return ESP_ERR_INVALID_ARG;
 static const char *const versions[]={"NKJV","ESV"};   /* NKJV preferred, ESV if it fails */
 esp_err_t e=ESP_FAIL;
 for(size_t i=0;i<sizeof(versions)/sizeof(versions[0]);i++){
  memset(out,0,sizeof(*out));
  if((e=verse_biblegateway(versions[i],out))==ESP_OK)return ESP_OK;
 }
 return e;
}

esp_err_t sf_net_get_bytes(const char *url,size_t max_bytes,uint8_t **data,size_t *len){
 if(!url||!data||!len)return ESP_ERR_INVALID_ARG;
 *data=NULL;*len=0;
 reply_t reply;esp_err_t e=fetch(url,max_bytes,&reply);if(e!=ESP_OK)return e;
 *data=(uint8_t*)reply.body;*len=reply.used;
 return ESP_OK;
}

/* ---- National Weather Service alerts (api.weather.gov) -----------------------
   GET /alerts/active?point=lat,lon lists the alerts in force for that spot as GeoJSON.
   The service asks for an identifying User-Agent, which fetch() sends. */
static int64_t days_from_civil(int y,int m,int d){   /* days since 1970-01-01 */
 y-=m<=2;
 int64_t era=(y>=0?y:y-399)/400;
 int64_t yoe=y-era*400, doy=(153*(m+(m>2?-3:9))+2)/5+d-1, doe=yoe*365+yoe/4-yoe/100+doy;
 return era*146097+doe-719468;
}
bool sf_parse_iso_time(const char *s,int64_t *utc,int *local_date,int *local_minute){
 int y,mo,d,h,mi,sec=0,oh=0,om=0,n=0;char sign=0;
 /* %n marks where the time really ends (the fields may be shorter than their usual width). */
 if(!s||sscanf(s,"%4d-%2d-%2dT%2d:%2d%n",&y,&mo,&d,&h,&mi,&n)<5||!n)return false;
 if(mo<1||mo>12||d<1||d>31||h>23||mi>59)return false;
 const char *z=s+n;
 int k=0;
 if(*z==':'&&sscanf(z,":%2d%n",&sec,&k)==1)z+=k;   /* seconds */
 if(*z=='.')while(isdigit((unsigned char)*++z));
 if(*z=='+'||*z=='-'){sign=*z;if(sscanf(z+1,"%2d:%2d",&oh,&om)<1)return false;}
 else if(*z!='Z'&&*z!=0)return false;
 int offset=(oh*60+om)*(sign=='-'?-1:1);
 if(utc)*utc=days_from_civil(y,mo,d)*86400+h*3600+mi*60+sec-(int64_t)offset*60;
 if(local_date)*local_date=y*10000+mo*100+d;
 if(local_minute)*local_minute=h*60+mi;
 return true;
}
/* NWS text is hard-wrapped at ~70 columns with blank lines between paragraphs: join the
   lines, keep the paragraphs (one '\n' each), fold to ASCII. Appends to dst. */
static void append_paragraphs(char *dst,size_t cap,const char *src){
 char *tmp=malloc(cap);
 if(!tmp||!src){free(tmp);return;}
 size_t n=0;
 for(const char *p=src;*p&&n+2<cap;p++){
  if(*p=='\r')continue;
  if(*p=='\n'){
   bool para=p[1]=='\n'||(p[1]=='\r'&&p[2]=='\n');
   while(p[1]=='\n'||p[1]=='\r')p++;
   if(n&&tmp[n-1]!='\n'&&tmp[n-1]!=' ')tmp[n++]=para?'\n':' ';
   else if(para&&n&&tmp[n-1]==' ')tmp[n-1]='\n';
   continue;
  }
  tmp[n++]=*p;
 }
 while(n&&(tmp[n-1]=='\n'||tmp[n-1]==' '))n--;
 tmp[n]=0;
 size_t used=strlen(dst);
 if(used&&used+2<cap){dst[used++]='\n';dst[used]=0;}
 sf_ascii_fold(tmp,dst+used,cap-used);   /* keeps '\n' */
 free(tmp);
}
static int alert_level(const char *event){
 if(strstr(event,"Warning")||strstr(event,"Emergency"))return 3;
 if(strstr(event,"Watch"))return 2;
 if(strstr(event,"Advisory"))return 1;
 return 0;
}
static int severity_rank(const char *s){
 return !s?0:!strcmp(s,"Extreme")?4:!strcmp(s,"Severe")?3:!strcmp(s,"Moderate")?2:!strcmp(s,"Minor")?1:0;
}
esp_err_t sf_net_get_wx_alerts(double lat,double lon,sf_wx_alerts_t *out){
 if(!out||!isfinite(lat)||!isfinite(lon))return ESP_ERR_INVALID_ARG;
 memset(out,0,sizeof(*out));
 char url[112];
 snprintf(url,sizeof(url),"https://api.weather.gov/alerts/active?status=actual&point=%.4f,%.4f",lat,lon);
 reply_t r;esp_err_t rc=fetch(url,SF_NET_ALERTS_LIMIT,&r);if(rc!=ESP_OK)return rc;
 cJSON *root=json_parse(r.body,r.used);free(r.body);
 cJSON *features=cJSON_GetObjectItemCaseSensitive(root,"features");
 if(!cJSON_IsArray(features)){cJSON_Delete(root);return ESP_ERR_INVALID_RESPONSE;}
 int ranks[SF_MAX_WX_ALERTS]={0};
 cJSON *f;
 cJSON_ArrayForEach(f,features){
  cJSON *p=cJSON_GetObjectItemCaseSensitive(f,"properties");
  cJSON *ev=cJSON_GetObjectItemCaseSensitive(p,"event");
  if(!cJSON_IsString(ev)||!ev->valuestring||!ev->valuestring[0])continue;
  cJSON *sev=cJSON_GetObjectItemCaseSensitive(p,"severity");
  int level=alert_level(ev->valuestring);
  int rank=level*10+severity_rank(cJSON_IsString(sev)?sev->valuestring:NULL);
  /* Overlapping zones can carry the same alert twice: one entry per event name. */
  bool dup=false;
  for(int i=0;i<out->count;i++){char e[48];sf_ascii_fold(ev->valuestring,e,sizeof(e));if(!strcmp(e,out->alert[i].event))dup=true;}
  if(dup)continue;
  out->total++;
  /* Keep the most serious SF_MAX_WX_ALERTS, in order. */
  int at=out->count;
  while(at>0&&ranks[at-1]<rank)at--;
  if(at>=SF_MAX_WX_ALERTS)continue;
  int last=out->count<SF_MAX_WX_ALERTS?out->count:SF_MAX_WX_ALERTS-1;
  memmove(&out->alert[at+1],&out->alert[at],(size_t)(last-at)*sizeof(out->alert[0]));
  memmove(&ranks[at+1],&ranks[at],(size_t)(last-at)*sizeof(ranks[0]));
  if(out->count<SF_MAX_WX_ALERTS)out->count++;
  sf_wx_alert_t *a=&out->alert[at];
  memset(a,0,sizeof(*a));ranks[at]=rank;
  sf_ascii_fold(ev->valuestring,a->event,sizeof(a->event));
  json_str(p,"areaDesc",a->area,sizeof(a->area));
  a->level=(uint8_t)level;
  a->ends_minute=-1;
  cJSON *ends=cJSON_GetObjectItemCaseSensitive(p,"ends");
  if(!cJSON_IsString(ends))ends=cJSON_GetObjectItemCaseSensitive(p,"expires");
  if(cJSON_IsString(ends))sf_parse_iso_time(ends->valuestring,&a->ends_utc,&a->ends_date,&a->ends_minute);
  cJSON *desc=cJSON_GetObjectItemCaseSensitive(p,"description"),*ins=cJSON_GetObjectItemCaseSensitive(p,"instruction");
  if(cJSON_IsString(desc))append_paragraphs(a->text,sizeof(a->text),desc->valuestring);
  if(cJSON_IsString(ins))append_paragraphs(a->text,sizeof(a->text),ins->valuestring);
 }
 cJSON_Delete(root);
 return ESP_OK;
}
