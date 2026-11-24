#include "skyframe_portal.h"
#include "skyframe_wifi.h"
#include "skyframe_nvs.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
static httpd_handle_t server;
static sf_config_t *current;
static sf_portal_saved_cb saved;
static char access_name[32], access_password[32];
void sf_portal_get_credentials(char *ssid,size_t ssid_size,char *password,size_t pass_size){
 if(ssid&&ssid_size)snprintf(ssid,ssid_size,"%s",access_name);
 if(password&&pass_size)snprintf(password,pass_size,"%s",access_password);
}
static void load_ap_password(void){
 nvs_handle_t h;
 if(nvs_open("skyframe",NVS_READWRITE,&h)!=ESP_OK) return;
 size_t length=sizeof(access_password);
 if(nvs_get_str(h,"setup_ap_pwd",access_password,&length)!=ESP_OK || length<13){
   static const char digits[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
   for(int i=0;i<16;i++)access_password[i]=digits[esp_random()%(sizeof(digits)-1)];
   access_password[16]=0;
   nvs_set_str(h,"setup_ap_pwd",access_password);
   nvs_commit(h);
 }
 nvs_close(h);
}
/* HTML escaping for values echoed into the page (Wi-Fi names, messages). */
static void html_escape(const char *in,char *out,size_t cap){
 size_t j=0;
 for(;*in&&j+7<cap;in++){
  const char *rep=NULL;
  switch(*in){case '&':rep="&amp;";break;case '<':rep="&lt;";break;case '>':rep="&gt;";break;
              case '"':rep="&quot;";break;case '\'':rep="&#39;";break;default:break;}
  if(rep){size_t n=strlen(rep);memcpy(out+j,rep,n);j+=n;}else out[j++]=*in;
 }
 out[j]=0;
}
/* One look for every page: sky-blue header, cards, large touch targets, and a dark
   variant that follows the phone's setting. Everything inline: the setup network has
   no internet. */
/* SkyFrame's logo on a 32-unit grid, the same design as apple-touch-icon.png (drawn by
   tools/make_touch_icon.py): a night-blue tile, radar rings, the sweep, and a jet crossing it.
   H is the color prefix: "#" in HTML, "%23" inside a data: URL. */
#define SF_LOGO(H) \
 "<rect width='32' height='32' rx='8' fill='" H "0b2340'/>" \
 "<path d='M16 16L9.2 5.6A12.44 12.44 0 0 1 28.3 14.3Z' fill='" H "38f0a8' fill-opacity='.4'/>" \
 "<path d='M16 16L28.3 14.3' stroke='" H "38f0a8' stroke-width='.9'/>" \
 "<circle cx='16' cy='16' r='12.4' fill='none' stroke='" H "5fd4ff' stroke-opacity='.85' stroke-width='1.2'/>" \
 "<circle cx='16' cy='16' r='8.3' fill='none' stroke='" H "5fd4ff' stroke-opacity='.4' stroke-width='.8'/>" \
 "<circle cx='16' cy='16' r='4.1' fill='none' stroke='" H "5fd4ff' stroke-opacity='.35' stroke-width='.8'/>" \
 "<path d='M24.8 7.2L24.8 7.9L24.2 8.8L21.6 11.4L23.6 17.6L23.0 18.3L19.7 13.3L17.4 15.4L17.6 17.8L17.0 18.4" \
 "L15.8 16.6L15.3 16.7L15.4 16.2L13.6 15.0L14.2 14.4L16.6 14.6L18.7 12.3L13.7 9.0L14.4 8.4L20.6 10.4L23.2 7.8" \
 "L24.1 7.2Z' fill='" H "fff'/>"
const char sf_portal_style[]=
"<meta name=viewport content='width=device-width,initial-scale=1'>"
/* Tab icon, inline so the browser needs no extra request. */
"<link rel=icon href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
SF_LOGO("%23") "</svg>\">"
"<link rel=apple-touch-icon href=/apple-touch-icon.png><meta name=apple-mobile-web-app-title content=SkyFrame>"
"<style>"
":root{--bg:#eef4f9;--card:#fff;--ink:#11232d;--mut:#5a6e7b;--line:#d5e1e9;--acc:#0a78b8;--on:#fff;"
"--soft:#e2f0f8;--ok:#16794a;--bad:#c0392b;color-scheme:light dark}"
"@media(prefers-color-scheme:dark){:root{--bg:#06141b;--card:#0d212b;--ink:#e6f1f6;--mut:#8ea5b2;"
"--line:#1d3846;--acc:#4cc4f0;--on:#03202c;--soft:#12313e;--ok:#5edc98;--bad:#ff8a7a}}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
"font:17px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;-webkit-text-size-adjust:100%}"
"header{background:linear-gradient(135deg,#0b3550,#0a78b8 60%,#39bdea);color:#fff;padding:16px 14px 0}"
".top,nav,main{max-width:560px;margin:auto}"
".top{display:flex;align-items:center;gap:10px;font-size:22px;font-weight:700;letter-spacing:.3px}"
"nav{display:flex;gap:4px;margin-top:12px}nav a{flex:1;text-align:center;padding:12px 4px;color:#fff;"
"text-decoration:none;font-weight:600;border-radius:12px 12px 0 0;opacity:.85}"
"nav a[aria-current]{background:var(--bg);color:var(--ink);opacity:1}"
"main{padding:16px 14px 32px}"
".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:0 0 16px;"
"box-shadow:0 1px 3px #0000000d}"
"h2{font-size:20px;margin:0 0 4px}h3{font-size:17px;margin:20px 0 0}.sub{color:var(--mut);margin:0 0 6px}"
"label{display:block;font-weight:600;margin-top:14px}"
"input:not([type=checkbox]){display:block;width:100%;margin-top:6px;font:inherit;padding:11px 12px;min-height:48px;"
"border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--ink)}"
"input:focus{outline:2px solid var(--acc);outline-offset:1px}"
".hint{display:block;font-weight:400;font-size:14px;color:var(--mut);margin-top:4px}"
".row{display:flex;gap:12px;flex-wrap:wrap}.row>label{flex:1;min-width:84px}"
".chk{display:flex;align-items:center;gap:10px;font-weight:500}"
"input[type=checkbox]{width:22px;height:22px;margin:0;accent-color:var(--acc)}"
"button,.btn{display:inline-block;font:inherit;font-weight:600;border:0;border-radius:12px;padding:12px 20px;"
"min-height:48px;background:var(--acc);color:var(--on);cursor:pointer;text-decoration:none}"
"button:active{transform:scale(.98)}.alt{background:var(--soft);color:var(--ink)}"
".bad{background:none;color:var(--bad);border:1px solid var(--line)}.sm{padding:8px 12px;min-height:40px;font-size:15px}"
".ok{color:var(--ok)}.err{color:var(--bad)}.note{display:block;margin-top:10px;font-weight:600}"
".bar{position:sticky;bottom:0;padding:14px 0 6px;background:linear-gradient(#0000,var(--bg) 35%)}"
".bar button{width:100%;font-size:18px}.bar .hint{text-align:center}"
".net{display:flex;align-items:center;gap:8px;padding:12px 0;border-bottom:1px solid var(--line)}"
".net .nm{flex:1;min-width:0;overflow-wrap:anywhere;font-weight:600}"
".tag{display:inline-block;font-size:13px;font-weight:600;background:var(--soft);color:var(--acc);"
"padding:2px 9px;border-radius:99px;margin-left:6px}.empty{color:var(--mut);font-style:italic}"
"</style>";
void sf_portal_page_start(httpd_req_t *r,const char *title,int tab){
 static const char *const tabs[][2]={{"/","Settings"},{"/photos","Photos"},{"/birthdays","Birthdays"}};
 char head[900];
 httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");
 snprintf(head,sizeof(head),"<!doctype html><html lang=en><meta charset=utf-8><title>%s</title>",title);
 httpd_resp_send_chunk(r,head,HTTPD_RESP_USE_STRLEN);
 httpd_resp_send_chunk(r,sf_portal_style,HTTPD_RESP_USE_STRLEN);
 httpd_resp_send_chunk(r,"<header><div class=top><svg width=34 height=34 viewBox='0 0 32 32' aria-hidden=true>"
  SF_LOGO("#") "</svg>SkyFrame</div><nav>",HTTPD_RESP_USE_STRLEN);
 int n=0;
 for(int i=0;i<3&&n>=0&&(size_t)n<sizeof(head);i++)
  n+=snprintf(head+n,sizeof(head)-n,"<a href='%s'%s>%s</a>",tabs[i][0],i==tab?" aria-current=page":"",tabs[i][1]);
 if(n>0&&(size_t)n<sizeof(head))snprintf(head+n,sizeof(head)-n,"</nav></header><main>");
 httpd_resp_send_chunk(r,head,HTTPD_RESP_USE_STRLEN);
}
esp_err_t sf_portal_page_end(httpd_req_t *r){
 httpd_resp_send_chunk(r,"</main></html>",HTTPD_RESP_USE_STRLEN);
 return httpd_resp_send_chunk(r,NULL,0);
}
/* Page text built in a PSRAM buffer; anything that doesn't fit marks it as overflowed
   (never a silently cut page). */
typedef struct {char *b;size_t n,cap;bool over;} out_t;
static void __attribute__((format(printf,2,3))) put(out_t *o,const char *fmt,...){
 if(o->over)return;
 va_list a;va_start(a,fmt);int k=vsnprintf(o->b+o->n,o->cap-o->n,fmt,a);va_end(a);
 if(k<0||(size_t)k>=o->cap-o->n){o->over=true;o->b[o->n]=0;return;}
 o->n+=(size_t)k;
}
/* The settings page shows the current values, so changing one setting doesn't mean
   re-entering the rest. Wi-Fi passwords are never sent back, only the network names.
   Remove / Prefer buttons belong to a separate empty form (form=nf), so pressing Enter
   in a field still means "Save settings" and they never carry the other fields. */
static esp_err_t page(httpd_req_t *r){
 if(!sf_portal_own_host(r))return sf_portal_to_own_host(r,"/");
 const size_t cap=12*1024;
 out_t o={heap_caps_malloc(cap,MALLOC_CAP_SPIRAM),0,cap,false};   /* keep internal RAM for Wi-Fi */
 if(!o.b){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");return ESP_OK;}
 int count=sf_config_network_count(current);
 put(&o,"<form id=nf method=post action='/save'></form><form method=post action='/save'>"
  "<section class=card><h2>Wi-Fi networks</h2>"
  "<p class=sub>SkyFrame joins the strongest saved network it can find, favoring the preferred one. "
  "It needs 2.4&nbsp;GHz Wi-Fi.</p>");
 if(!count)put(&o,"<p class=empty>No networks saved yet. Add your home Wi-Fi below.</p>");
 for(int i=0;i<count;i++){
  char name[200];html_escape(current->wifi[i].ssid,name,sizeof(name));
  put(&o,"<div class=net><span class=nm>%s%s</span>",name,i?"":"<span class=tag>Preferred</span>");
  if(i)put(&o,"<button class='sm alt' form=nf name=prefer value=\"%s\">Prefer</button>",name);
  put(&o,"<button class='sm bad' form=nf name=remove value=\"%s\" "
   "onclick=\"return confirm('Forget this network? SkyFrame will restart.')\">Remove</button></div>",name);
 }
 put(&o,"<h3>%s</h3>"
  "<label>Network name<input name=ssid maxlength=32 autocomplete=off autocapitalize=none spellcheck=false%s>"
  "<span class=hint>Exactly as your phone's Wi-Fi list shows it (capital letters matter).</span></label>"
  "<label>Password<input id=pw name=password type=password maxlength=64 autocomplete=new-password>"
  "<span class=hint>Leave empty for an open network. To change a saved password, type that network's name "
  "and the new password.</span></label>"
  "<label class=chk><input type=checkbox onclick=\"document.getElementById('pw').type=this.checked?'text':'password'\">"
  "Show password</label></section>",
  count?"Add a network":"Add your Wi-Fi",count?"":" required");
 put(&o,"<section class=card><h2>Radar</h2><p class=sub>The center of the radar, usually your home.</p>"
  "<div class=row><label>Latitude<input name=lat type=number step=any required value='%.5f'></label>"
  "<label>Longitude<input name=lon type=number step=any required value='%.5f'></label></div>"
  "<span class=hint>Tip: in Google Maps, press and hold on your home; the two numbers appear at the top.</span>"
  "<label>Radar radius (miles)<input name=display type=number min=1 max=100 value='%u'>"
  "<span class=hint>How far out the radar shows planes, 1 to 100.</span></label>"
  "<label>Show the radar when a plane is within (miles)<input name=activation type=number min=1 max=100 value='%u'>"
  "<span class=hint>A plane this close switches the screen to the radar. At most the radar radius.</span></label></section>",
  current->latitude,current->longitude,current->display_miles,current->activation_miles);
 put(&o,"<section class=card><h2>Screen times</h2><p class=sub>Seconds each screen stays up before the next one.</p>"
  "<div class=row><label>Radar<input name=radar type=number min=30 max=3600 value='%u'><span class=hint>30 to 3600</span></label>"
  "<label>Weather<input name=weather type=number min=5 max=600 value='%u'><span class=hint>5 to 600</span></label>"
  "<label>Photos<input name=photos type=number min=10 max=3600 value='%u'><span class=hint>10 to 3600</span></label></div>"
  "<div class=row><label>Verse of the day<input name=verse type=number min=0 max=600 value='%u'><span class=hint>5 to 600, 0 = off</span></label>"
  "<label>Today's sky<input name=today type=number min=0 max=600 value='%u'><span class=hint>5 to 600, 0 = off</span></label></div>"
  "<input type=hidden name=ui value=1>"
  "<label class=chk><input type=checkbox name=transitions value=1%s>Animated page transitions</label>"
  "<span class=hint>The radar's last sweep, a jet flying over, the book opening... Off: screens change instantly.</span></section>",
  current->radar_seconds,current->weather_seconds,current->photo_seconds,current->verse_seconds,current->today_seconds,
  current->ui_flags&SF_UI_NO_TRANSITIONS?"":" checked");
 put(&o,"<section class=card><h2>Night dimming</h2><p class=sub>The screen stays on, dimmed, between these hours. "
  "A tap brightens it for a minute. Use the same hour twice to turn this off.</p>"
  "<div class=row><label>Dim at<input name=night_start type=number min=0 max=23 value='%u'>"
  "<span class=hint>Hour 0 to 23 (22 = 10 pm)</span></label>"
  "<label>Full brightness at<input name=night_end type=number min=0 max=23 value='%u'>"
  "<span class=hint>Hour 0 to 23 (7 = 7 am)</span></label></div></section>",
  current->night_start_hour,current->night_end_hour);
 char types[sizeof(current->alert_types)*6];html_escape(current->alert_types,types,sizeof(types));
 put(&o,"<section class=card><h2>Sighting alerts</h2><p class=sub>A banner pops up over any screen, with a pulse "
  "around the edge, when one of these shows up on the radar. Tap the banner to see it on the radar.</p>"
  "<input type=hidden name=alerts value=1>"
  "<label class=chk><input type=checkbox name=alert_emergency value=1%s>Emergencies (squawk 7700, 7600, 7500)</label>"
  "<label class=chk><input type=checkbox name=alert_vip value=1%s>Air Force One and government VIP flights</label>"
  "<label class=chk><input type=checkbox name=alert_military value=1%s>Military aircraft (not helicopters)</label>"
  "<label class=chk><input type=checkbox name=alert_overhead value=1%s>Any plane passing overhead (within about a mile), briefly</label>"
  "<label>These aircraft types<input name=alert_types maxlength=60 autocapitalize=characters spellcheck=false "
  "value=\"%s\" placeholder='B74*, A388'><span class=hint>ICAO type codes, separated by commas: B748 is a 747-8, "
  "B74* any 747, A388 an A380, C17 a C-17. Leave empty for none.</span></label></section>"
  "<div class=bar><button type=submit>Save settings</button>"
  "<span class=hint>SkyFrame restarts after saving; this takes about 20 seconds.</span></div></form>",
  current->alert_flags&SF_ALERT_EMERGENCY?" checked":"",current->alert_flags&SF_ALERT_VIP?" checked":"",
  current->alert_flags&SF_ALERT_MILITARY?" checked":"",current->alert_flags&SF_ALERT_NO_OVERHEAD?"":" checked",types);
 if(o.over){free(o.b);httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Page too large");return ESP_OK;}
 sf_portal_page_start(r,"SkyFrame settings",SF_TAB_SETTINGS);
 httpd_resp_send_chunk(r,o.b,(ssize_t)o.n);
 free(o.b);
 return sf_portal_page_end(r);
}
/* Result of a save, in the same frame as the pages, with a clear way back. */
static esp_err_t result(httpd_req_t *r,const char *status,const char *heading,const char *text,bool ok){
 char safe[400],body[900];
 html_escape(text,safe,sizeof(safe));
 if(ok)snprintf(body,sizeof(body),"<meta http-equiv=refresh content='25;url=/'><section class=card>"
  "<h2 class=ok>&#10003; %s</h2><p>%s</p><a class=btn href='/'>Back to settings</a></section>",heading,safe);
 else snprintf(body,sizeof(body),"<section class=card><h2 class=err>%s</h2><p>%s</p>"
  "<a class=btn href='/' onclick='history.back();return false'>Go back and fix it</a></section>",heading,safe);
 httpd_resp_set_status(r,status);
 sf_portal_page_start(r,"SkyFrame settings",-1);
 httpd_resp_send_chunk(r,body,HTTPD_RESP_USE_STRLEN);
 return sf_portal_page_end(r);
}
/* The radar icon as a PNG: iPhones ask for it at fixed paths for bookmarks and "Add to Home
   Screen" (they ignore the inline SVG icon), and other browsers' /favicon.ico gets it too. */
extern const uint8_t touch_icon_start[] asm("_binary_apple_touch_icon_png_start");
extern const uint8_t touch_icon_end[] asm("_binary_apple_touch_icon_png_end");
static esp_err_t touch_icon(httpd_req_t *r){
 httpd_resp_set_type(r,"image/png");
 httpd_resp_set_hdr(r,"Cache-Control","max-age=604800");
 return httpd_resp_send(r,(const char*)touch_icon_start,touch_icon_end-touch_icon_start);
}
static esp_err_t to_settings(httpd_req_t *r){
 httpd_resp_set_status(r,"303 See Other");httpd_resp_set_hdr(r,"Location","/");
 return httpd_resp_send(r,NULL,0);
}
static esp_err_t save(httpd_req_t *r){
 if(!sf_portal_same_origin(r)){httpd_resp_send_err(r,HTTPD_403_FORBIDDEN,"Use the SkyFrame page to change settings");return ESP_OK;}
 if(r->content_len<=0||r->content_len>2048){httpd_resp_send_err(r,HTTPD_413_CONTENT_TOO_LARGE,"Invalid size");return ESP_OK;}
 char body[2049];int got=0;
 while(got<(int)r->content_len){int n=httpd_req_recv(r,body+got,r->content_len-got);if(n<=0){httpd_resp_send_err(r,HTTPD_408_REQ_TIMEOUT,"Timeout");return ESP_OK;}got+=n;}
 body[got]=0;sf_config_t next=*current;const char *why=NULL;
 if(!sf_config_apply_form(&next,body,got,&why))
  return result(r,"400 Bad Request","Not saved",why?why:"Some settings weren't accepted. Please check them and try again.",false);
 if(sf_config_nvs_save(&next)!=ESP_OK)
  return result(r,"500 Internal Server Error","Not saved","SkyFrame couldn't store the settings. Please try again.",false);
 *current=next;
 const char *done=!strncmp(body,"remove=",7)?"Network removed":!strncmp(body,"prefer=",7)?"Preferred network changed":"Settings saved";
 result(r,"200 OK",done,"SkyFrame is restarting with the new settings. This page comes back by itself in about 25 seconds.",true);
 if(saved)saved(current);
 return ESP_OK;
}
void sf_portal_set_ap(bool on){
 wifi_mode_t mode;
 if(esp_wifi_get_mode(&mode)!=ESP_OK)return;
 wifi_mode_t want=on?WIFI_MODE_APSTA:WIFI_MODE_STA;
 if(mode!=want){
  esp_wifi_set_mode(want);
  ESP_LOGI("skyframe","Setup network %s",on?"on":"off");
  /* Changing mode cancels a connection attempt in progress without a disconnect
     event, so the retry logic never runs: start a fresh attempt. */
  sf_wifi_kick();
 }
}
esp_err_t sf_portal_start(sf_config_t *config,sf_portal_saved_cb callback){
 if(!config)return ESP_ERR_INVALID_ARG;
 current=config;saved=callback;
 /* Setup network: name from the MAC; WPA2 password random per device, kept in NVS
    and shown on the setup screen. Convenience onboarding, not high-assurance security. */
 uint8_t mac[6];ESP_RETURN_ON_ERROR(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP),"skyframe","mac");
 snprintf(access_name,sizeof(access_name),"SkyFrame-%02X%02X",mac[4],mac[5]);
 load_ap_password();
 if(strlen(access_password)<8) return ESP_ERR_INVALID_STATE;
 wifi_config_t ap={0};snprintf((char*)ap.ap.ssid,sizeof(ap.ap.ssid),"%s",access_name);
 snprintf((char*)ap.ap.password,sizeof(ap.ap.password),"%s",access_password);
 ap.ap.ssid_len=strlen((char*)ap.ap.ssid);ap.ap.authmode=WIFI_AUTH_WPA2_PSK;ap.ap.max_connection=2;
 /* Start on the home router's channel, so joining it doesn't move the setup network. */
 if(sf_wifi_home_channel())ap.ap.channel=sf_wifi_home_channel();
 ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),"skyframe","APSTA");
 ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP,&ap),"skyframe","AP config");
 ESP_RETURN_ON_ERROR(esp_wifi_start(),"skyframe","WiFi start");
 /* Serves on both the setup network and the home network. */
 httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();cfg.stack_size=8192;cfg.max_uri_handlers=20;
 cfg.lru_purge_enable=true;
 ESP_RETURN_ON_ERROR(httpd_start(&server,&cfg),"skyframe","HTTP start");
 httpd_uri_t get={.uri="/",.method=HTTP_GET,.handler=page};
 httpd_uri_t post={.uri="/save",.method=HTTP_POST,.handler=save};
 /* Reloading the result page (or going back to it) asks for /save again: send it home. */
 httpd_uri_t again={.uri="/save",.method=HTTP_GET,.handler=to_settings};
 httpd_register_uri_handler(server,&get);httpd_register_uri_handler(server,&post);
 httpd_register_uri_handler(server,&again);
 static const char *const icon_uris[]={"/apple-touch-icon.png","/apple-touch-icon-precomposed.png","/favicon.ico"};
 for(size_t i=0;i<sizeof(icon_uris)/sizeof(icon_uris[0]);i++){
  httpd_uri_t icon={.uri=icon_uris[i],.method=HTTP_GET,.handler=touch_icon};
  httpd_register_uri_handler(server,&icon);
 }
 sf_photo_web_register(server);
 sf_birthday_web_register(server);
 ESP_LOGI("skyframe","Setup AP %s available at http://192.168.4.1",access_name);
 return ESP_OK;
}
void sf_portal_stop(void){if(server){httpd_stop(server);server=NULL;}}
