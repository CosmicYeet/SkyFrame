#include "skyframe_wifi.h"
#include "skyframe_portal.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>
static sf_config_t *settings;
static volatile bool connected, ever_connected;
static volatile uint32_t failures;
static bool started;
static esp_timer_handle_t retry_timer, watchdog_timer;
static volatile int64_t attempt_at;     /* when the last connection attempt started (us) */
static uint32_t backoff_ms;
static const char *TAG="skyframe_wifi";
#define RETRY_FIRST_MS 2000u
#define RETRY_MAX_MS 60000u
/* The home router's channel, saved after each connect. On boot the setup network starts
   on it and the first attempt joins on it directly: otherwise the setup network starts on
   channel 1 and has to move mid-join, which makes the first attempt fail. The channel
   belongs to one network (CHANNEL_NET_KEY), so it is only used when joining that one. */
#define CHANNEL_KEY "wifi_channel"
#define CHANNEL_NET_KEY "wifi_chan_net"
static uint8_t home_channel;           /* 0: unknown */
static char channel_ssid[SF_WIFI_SSID_MAX+1];
static bool channel_hint;              /* STA config currently names home_channel */
static bool hint_ok=true;              /* cleared once an attempt with the hint fails */
static void load_channel(void){
 nvs_handle_t h;uint8_t ch=0;size_t len=sizeof(channel_ssid);
 channel_ssid[0]=0;
 if(nvs_open("skyframe",NVS_READONLY,&h)==ESP_OK){
  nvs_get_u8(h,CHANNEL_KEY,&ch);
  if(nvs_get_str(h,CHANNEL_NET_KEY,channel_ssid,&len)!=ESP_OK)channel_ssid[0]=0;
  nvs_close(h);
 }
 /* Saved by firmware that knew only one network: it was learned on that one. */
 if(!channel_ssid[0])snprintf(channel_ssid,sizeof(channel_ssid),"%s",settings->wifi[0].ssid);
 home_channel=ch>=1&&ch<=13?ch:0;
}
static void save_channel(uint8_t ch,const char *ssid){
 if(ch<1||ch>13||(ch==home_channel&&!strcmp(ssid,channel_ssid)))return;
 nvs_handle_t h;
 if(nvs_open("skyframe",NVS_READWRITE,&h)!=ESP_OK)return;
 if(nvs_set_u8(h,CHANNEL_KEY,ch)==ESP_OK&&nvs_set_str(h,CHANNEL_NET_KEY,ssid)==ESP_OK)nvs_commit(h);
 nvs_close(h);
 home_channel=ch;snprintf(channel_ssid,sizeof(channel_ssid),"%s",ssid);
}
uint8_t sf_wifi_home_channel(void){return home_channel;}

/* Saved networks to try, best first: the ones the last scan saw (strongest first, the
   preferred one favored), then the rest in list order (hidden or out of range). Each
   failed attempt moves on to the next; after a full round, a new scan if the last one
   is old. With a single saved network there is nothing to choose, so no scans at all. */
#define PREFERRED_BONUS_DB 10
#define RESCAN_US (120LL*1000000LL)
static uint8_t order[SF_WIFI_NETWORKS], order_n, cur;
static uint8_t visible_n, cur_tries;   /* candidates the last scan saw; retries on the current one */
static volatile bool scanning;
static bool rescan;                    /* look again first (after losing a connection) */
static int64_t scanned_at;             /* 0: never */
static int configured_slot=-1;         /* what the STA config currently holds */
static bool configured_hint;
static int networks(void){return settings?sf_config_network_count(settings):0;}
static void default_order(void){
 order_n=(uint8_t)networks();
 for(uint8_t i=0;i<order_n;i++)order[i]=i;
 cur=0;visible_n=0;cur_tries=0;
}
static void configure_sta(int slot,bool hint){
 hint=hint&&home_channel;
 /* Rewriting an unchanged config is skipped: it can disturb an attempt in progress. */
 if(slot==configured_slot&&hint==configured_hint)return;
 const sf_wifi_network_t *w=&settings->wifi[slot];
 wifi_config_t sta={0};
 memcpy(sta.sta.ssid,w->ssid,strlen(w->ssid));
 memcpy(sta.sta.password,w->password,strlen(w->password));
 /* With a password, never join an open look-alike network of the same name. */
 sta.sta.threshold.authmode=w->password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
 channel_hint=hint;
 if(channel_hint)sta.sta.channel=home_channel;
 esp_wifi_set_config(WIFI_IF_STA,&sta);
 configured_slot=slot;configured_hint=hint;
}
/* Connect to the current candidate. */
static void join(void){
 if(!order_n)default_order();
 if(!order_n)return;
 if(cur>=order_n)cur=0;
 int slot=order[cur];
 configure_sta(slot,hint_ok&&!strcmp(settings->wifi[slot].ssid,channel_ssid));
 attempt_at=esp_timer_get_time();
 esp_wifi_connect();
}
/* After a failed attempt. A network the scan saw gets a second try first: the first WPA3 join
   after boot is often refused, and moving on to a network that isn't in range costs seconds. */
static void next_candidate(void){
 if(cur<visible_n&&!cur_tries){cur_tries=1;return;}
 cur_tries=0;
 if(++cur>=order_n)cur=0;
}
/* Start an attempt: a scan first when it's worth it (it connects when done), else join. */
static void attempt(void){
 int n=networks();
 if(!n)return;
 if((int)order_n!=n)default_order();
 int64_t now=esp_timer_get_time();
 attempt_at=now;
 if(n>1&&(rescan||!scanned_at||(cur==0&&now-scanned_at>=RESCAN_US))){
  wifi_scan_config_t sc={0};           /* every channel, active, default dwell times */
  scanning=true;rescan=false;scanned_at=now;
  if(esp_wifi_scan_start(&sc,false)==ESP_OK)return;
  scanning=false;
 }
 join();
}
/* Rank saved networks by what the scan heard. Runs on the event task: keep it small. */
static void use_scan(void){
 int8_t rssi[SF_WIFI_NETWORKS]={0};bool seen[SF_WIFI_NETWORKS]={0};
 wifi_ap_record_t ap;
 while(esp_wifi_scan_get_ap_record(&ap)==ESP_OK){
  int i=sf_config_find_network(settings,(const char*)ap.ssid);
  if(i>=0&&(!seen[i]||ap.rssi>rssi[i])){seen[i]=true;rssi[i]=ap.rssi;}
 }
 esp_wifi_clear_ap_list();
 int n=networks();order_n=0;cur=0;cur_tries=0;
 for(int i=0;i<n;i++)if(seen[i]){
  int score=rssi[i]+(i==0?PREFERRED_BONUS_DB:0),k=order_n++;
  while(k>0){int p=order[k-1];if(rssi[p]+(p==0?PREFERRED_BONUS_DB:0)>=score)break;order[k]=order[k-1];k--;}
  order[k]=(uint8_t)i;
 }
 int visible=order_n;visible_n=(uint8_t)visible;
 for(int i=0;i<n;i++)if(!seen[i])order[order_n++]=(uint8_t)i;
 if(visible)ESP_LOGI(TAG,"%d of %d saved networks in range; trying \"%s\" first",visible,n,settings->wifi[order[0]].ssid);
 else ESP_LOGI(TAG,"No saved network seen; trying each in turn");
}
/* A restart SkyFrame chose (settings saved, test menu). The chip can finish one through its
   backup watchdog, which then reads as a freeze; this mark, kept in RTC memory across the
   restart, tells the next boot it was intended. */
#define PLANNED_MAGIC 0x53465253u
static RTC_NOINIT_ATTR uint32_t planned_mark;
void sf_planned_restart(void){
 /* Someone chose this restart (settings saved, test menu) on a new version that hasn't
    proven itself yet: it runs well enough to be used, so keep it. Going back now could also
    leave the previous version with settings it can't read. */
 const esp_partition_t *run=esp_ota_get_running_partition();esp_ota_img_states_t st;
 if(run&&esp_ota_get_state_partition(run,&st)==ESP_OK&&st==ESP_OTA_IMG_PENDING_VERIFY)
  esp_ota_mark_app_valid_cancel_rollback();
 planned_mark=PLANNED_MAGIC;esp_restart();
}
bool sf_restart_was_planned(void){
 static int planned=-1;               /* read once: the mark is cleared for the next boot */
 if(planned<0){planned=planned_mark==PLANNED_MAGIC&&esp_reset_reason()!=ESP_RST_POWERON;planned_mark=0;}
 return planned;
}
static void reboot_after_config(void *ctx){(void)ctx;
  vTaskDelay(pdMS_TO_TICKS(1500));sf_planned_restart();vTaskDelete(NULL);
}
static void saved(const sf_config_t *cfg){(void)cfg;
  /* Reload a complete settings snapshot on reboot after HTTP response returns. */
  xTaskCreate(reboot_after_config,"skyframe_restart",2048,NULL,4,NULL);
}
static void retry(void *arg){(void)arg;if(!connected)attempt();}
/* Start a new attempt now, e.g. after a Wi-Fi mode change, which cancels an attempt in
   progress without reporting a disconnect. A scan in progress is dropped (a mode change
   may have ended it silently too): join the current candidate directly. */
void sf_wifi_kick(void){
 if(connected||!networks())return;
 if(scanning){scanning=false;esp_wifi_scan_stop();}
 join();
}
/* Safety net: down, no retry scheduled, and the last attempt is long over (it must have
   been lost without a disconnect event): try again rather than sit idle forever. */
#define STALL_US (30LL*1000000LL)
static volatile bool own_disconnect;   /* the watchdog's reset, not a real failure */
static void watchdog(void *arg){
 (void)arg;
 if(connected||!networks()||!retry_timer||esp_timer_is_active(retry_timer))return;
 if(esp_timer_get_time()-attempt_at<STALL_US)return;
 ESP_LOGW(TAG,"Wi-Fi stuck while disconnected; resetting the connection");
 failures++;
 /* The driver can believe an attempt is still "connecting" after it was cancelled;
    disconnect clears that state so the new attempt actually starts. */
 own_disconnect=true;
 esp_wifi_disconnect();
 if(channel_hint)hint_ok=false;        /* stuck: search every channel from here on */
 if(!scanning)next_candidate();        /* a lost scan isn't the network's fault */
 sf_wifi_kick();
}
static void events(void *arg, esp_event_base_t base,int32_t id,void *data){
 (void)arg;(void)data;
 if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){
  connected=true;failures=0;backoff_ms=RETRY_FIRST_MS;
  if(!ever_connected){
   /* Internet time for the clock and night schedule; SNTP keeps it synced from here on. */
   esp_sntp_config_t sntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
   esp_netif_sntp_init(&sntp);
  }
  ever_connected=true;
  wifi_ap_record_t ap;
  if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK)save_channel(ap.primary,(const char*)ap.ssid);
  ESP_LOGI(TAG,"Station connected");
 }
 if(base==WIFI_EVENT&&id==WIFI_EVENT_SCAN_DONE){
  if(!scanning||connected){esp_wifi_clear_ap_list();return;}   /* stale or cancelled */
  scanning=false;
  use_scan();
  join();
 }
 if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){
  bool was_connected=connected;
  connected=false;
  /* The disconnect the watchdog itself asked for arrives after its new attempt started:
     already counted, and must not reconfigure or schedule over that attempt. */
  if(own_disconnect){own_disconnect=false;return;}
  failures++;
  /* The router may have moved to another channel: retries search all channels. */
  if(channel_hint)hint_ok=false;
  /* Lost a working connection: look around again (maybe another saved network is now
     closer). A failed attempt: on to the next candidate. */
  if(was_connected){rescan=true;cur=0;cur_tries=0;}
  else next_candidate();
  /* Back off instead of retrying instantly: constant scanning in AP+STA mode keeps
     moving the setup network's channel, making the setup page unreliable. */
  if(retry_timer&&!esp_timer_is_active(retry_timer))esp_timer_start_once(retry_timer,(uint64_t)backoff_ms*1000u);
  backoff_ms=backoff_ms*2>RETRY_MAX_MS?RETRY_MAX_MS:backoff_ms*2;
 }
}
bool sf_wifi_is_connected(void){return connected;}
bool sf_wifi_ip(char *out,size_t cap){
 esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
 esp_netif_ip_info_t ip;
 if(!out||!cap||!connected||!sta||esp_netif_get_ip_info(sta,&ip)!=ESP_OK||!ip.ip.addr)return false;
 snprintf(out,cap,IPSTR,IP2STR(&ip.ip));
 return true;
}
bool sf_wifi_needs_setup(void){
 /* Saved settings that have not connected once since boot after several attempts
    (about 30 s of retries, each saved network tried about twice) most likely mean
    wrong names or passwords, or none of the networks is in range. */
 int n=networks();
 uint32_t enough=n*2>4?(uint32_t)n*2:4u;
 return n&&!ever_connected&&failures>=enough;
}
esp_err_t sf_wifi_begin(sf_config_t *cfg){
 if (!cfg) {
  return ESP_ERR_INVALID_ARG;
 }
 if (started) {
  return ESP_ERR_INVALID_STATE;
 }
 settings=cfg;
 backoff_ms=RETRY_FIRST_MS;
 load_channel();                       /* before the portal sets up the setup network */
 default_order();
 ESP_ERROR_CHECK(esp_netif_init());
 ESP_ERROR_CHECK(esp_event_loop_create_default());
 esp_netif_create_default_wifi_sta();esp_netif_create_default_wifi_ap();
 wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
 ESP_ERROR_CHECK(esp_wifi_init(&init));
 /* Settings come from SkyFrame's own storage; don't also write every Wi-Fi config change
    (e.g. the channel hint on and off) to flash. */
 esp_wifi_set_storage(WIFI_STORAGE_RAM);
 const esp_timer_create_args_t timer_args={.callback=retry,.name="wifi_retry"};
 ESP_ERROR_CHECK(esp_timer_create(&timer_args,&retry_timer));
 const esp_timer_create_args_t watchdog_args={.callback=watchdog,.name="wifi_watchdog"};
 ESP_ERROR_CHECK(esp_timer_create(&watchdog_args,&watchdog_timer));
 esp_timer_start_periodic(watchdog_timer,10LL*1000000LL);
 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,events,NULL));
 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,events,NULL));
 ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,events,NULL));
 ESP_ERROR_CHECK(sf_portal_start(cfg,saved));
 /* http://skyframe.local on the home network (settings and photo uploads). */
 if(mdns_init()==ESP_OK){
  mdns_hostname_set("skyframe");
  mdns_instance_name_set("SkyFrame");
  mdns_service_add(NULL,"_http","_tcp",80,NULL,0);
 }else ESP_LOGW(TAG,"mDNS unavailable; use the IP address instead");
 attempt();started=true;
 return ESP_OK;
}
