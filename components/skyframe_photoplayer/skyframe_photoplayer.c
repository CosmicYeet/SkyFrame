#include "skyframe_photoplayer.h"
#include "skyframe_jpeg.h"
#include "skyframe_screens.h"
#include "skyframe_photos.h"
#include "skyframe_sd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
/* Decoding a phone photo takes seconds, so a worker task on CPU1 decodes a few
   photos ahead (so swipes turn at once) and the LVGL task only swaps in a ready frame.
   Double-buffer images across transitions: a frame is not freed until the slot
   is reused two photos later (10+ s after it stopped being shown). */
static const char *const photo_dirs[] = {SF_SD_ROOT "/photos", SF_SD_ROOT "/pictures"};
static sf_photo_catalog_t *catalog;    /* 64 KB of paths: PSRAM, not internal RAM */
static struct { uint16_t *pixels; lv_img_dsc_t dsc; } slots[2];
static unsigned next_slot;
static const char *TAG="skyframe_photo";

static portMUX_TYPE ready_lock=portMUX_INITIALIZER_UNLOCKED;
#define READY_MAX 2                    /* decoded frames waiting (460 KB each, PSRAM) */
static uint16_t *ready[READY_MAX];     /* FIFO: decoded, waiting for the UI; owned by whoever removes it */
static unsigned ready_count;
static volatile sf_photo_status_t status=SF_PHOTOS_LOADING;
static TaskHandle_t worker;

/* (Re)build the catalog from the first folder that holds photos. Card must be held. */
static void rescan(void){
 catalog->count=0;catalog->next=0;
 for(size_t i=0;i<sizeof(photo_dirs)/sizeof(photo_dirs[0]);i++){
  if(sf_photos_scan(catalog,photo_dirs[i])&&catalog->count){
   catalog->rng=esp_random()|1u;      /* a different shuffle every boot and card change */
   ESP_LOGI(TAG,"Found %u photo(s) in %s",(unsigned)catalog->count,photo_dirs[i]);
   return;
  }
 }
 ESP_LOGW(TAG,"No .jpg photos in %s or %s",photo_dirs[0],photo_dirs[1]);
}

static void decode_worker(void *arg){
 (void)arg;
 unsigned failures=0;
 uint32_t scanned=0;
 for(;;){
  /* Card ownership: rescan whenever it (re)mounts, e.g. after a USB transfer. */
  if(!sf_sd_present()){status=SF_PHOTOS_NO_CARD;vTaskDelay(pdMS_TO_TICKS(5000));continue;}
  if(sf_sd_usb_active()){status=SF_PHOTOS_USB;vTaskDelay(pdMS_TO_TICKS(500));continue;}
  uint32_t gen=sf_sd_generation();
  if(gen!=scanned){
   if(!sf_sd_begin()){vTaskDelay(pdMS_TO_TICKS(500));continue;}
   rescan();
   sf_sd_end();
   /* Frames decoded from the card as it was (perhaps a photo since deleted) go. */
   uint16_t *stale[READY_MAX];unsigned n_stale;
   taskENTER_CRITICAL(&ready_lock);
   n_stale=ready_count<READY_MAX?ready_count:READY_MAX;memcpy(stale,ready,sizeof(stale));
   memset(ready,0,sizeof(ready));ready_count=0;
   taskEXIT_CRITICAL(&ready_lock);
   for(unsigned i=0;i<n_stale;i++)free(stale[i]);
   scanned=gen;failures=0;
   status=catalog->count?SF_PHOTOS_LOADING:SF_PHOTOS_NONE;
  }
  if(!catalog->count){vTaskDelay(pdMS_TO_TICKS(1000));continue;}

  taskENTER_CRITICAL(&ready_lock);
  bool full=ready_count>=READY_MAX;
  taskEXIT_CRITICAL(&ready_lock);
  /* Wait until the UI takes a frame; wake periodically to notice card changes. */
  if(full){ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));continue;}
  /* A frame is still waiting, so nobody is held up: let the transition that just started
     finish before competing with it for PSRAM bandwidth. A swipe that emptied the queue
     gets the next decode at once. */
  taskENTER_CRITICAL(&ready_lock);
  bool spare=ready_count>0;
  taskEXIT_CRITICAL(&ready_lock);
  if(spare)vTaskDelay(pdMS_TO_TICKS(1200));

  const char *path=sf_photos_next(catalog);
  uint16_t *frame=NULL;
  uint8_t *jpeg=NULL;size_t len=0;
  esp_err_t e=ESP_ERR_INVALID_STATE;
  /* Hold the card only while reading; decode from memory so a USB handover never waits on it. */
  if(path&&sf_sd_begin()){
   e=sf_jpeg_read_file(path,&jpeg,&len);
   sf_sd_end();
  }
  if(e==ESP_ERR_INVALID_STATE){vTaskDelay(pdMS_TO_TICKS(500));continue;}   /* card went away */
  if(e==ESP_OK){e=sf_jpeg_decode_fit(jpeg,len,&frame,480,480,path);free(jpeg);}
  if(e==ESP_ERR_NO_MEM){   /* memory is busy right now (e.g. a plane photo); not the photo's fault */
   ESP_LOGW(TAG,"Not enough memory for %s right now; trying again shortly",path);
   vTaskDelay(pdMS_TO_TICKS(2000));continue;
  }
  if(e!=ESP_OK){
   ESP_LOGW(TAG,"Skipping %s: %s (needs baseline JPEG, <=4 MB)",path?path:"(none)",esp_err_to_name(e));
   if(++failures>=catalog->count){
    status=SF_PHOTOS_UNREADABLE;failures=0;
    vTaskDelay(pdMS_TO_TICKS(60000));   /* try the set again later */
   }
   continue;
  }
  failures=0;status=SF_PHOTOS_OK;
  taskENTER_CRITICAL(&ready_lock);
  ready[ready_count++]=frame;
  taskEXIT_CRITICAL(&ready_lock);
 }
}

/* LVGL task: never blocks. NULL means no new frame yet; *state says why. */
static const lv_img_dsc_t *next_picture(void *ctx,sf_photo_status_t *state){
 (void)ctx;
 if(state)*state=status;
 taskENTER_CRITICAL(&ready_lock);
 uint16_t *frame=NULL;
 if(ready_count){
  frame=ready[0];
  for(unsigned i=1;i<ready_count;i++)ready[i-1]=ready[i];
  ready[--ready_count]=NULL;
 }
 taskEXIT_CRITICAL(&ready_lock);
 if(!frame)return NULL;
 if(worker)xTaskNotifyGive(worker);    /* start decoding the one after */
 unsigned idx=next_slot;next_slot^=1;
 /* Same descriptor, new pixels: make sure no image cache keeps the old pointer. */
 lv_img_cache_invalidate_src(&slots[idx].dsc);
 free(slots[idx].pixels);slots[idx].pixels=frame;
 memset(&slots[idx].dsc,0,sizeof(slots[idx].dsc));
 slots[idx].dsc.header.cf=LV_IMG_CF_TRUE_COLOR;
 slots[idx].dsc.header.w=480;slots[idx].dsc.header.h=480;
 slots[idx].dsc.data_size=480u*480u*2u;
 slots[idx].dsc.data=(const uint8_t*)frame;
 return &slots[idx].dsc;
}

esp_err_t sf_photoplayer_start(void){
 if(worker)return ESP_ERR_INVALID_STATE;
 catalog=heap_caps_calloc(1,sizeof(*catalog),MALLOC_CAP_SPIRAM);
 if(!catalog)return ESP_ERR_NO_MEM;
 next_slot=0;
 if(xTaskCreatePinnedToCore(decode_worker,"skyframe_photo",6144,NULL,2,&worker,1)!=pdPASS){
  worker=NULL;free(catalog);catalog=NULL;return ESP_ERR_NO_MEM;
 }
 sf_photo_set_provider(next_picture,NULL);
 return ESP_OK;
}
