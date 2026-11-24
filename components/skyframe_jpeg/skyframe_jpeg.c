#include "skyframe_jpeg.h"
#include "skyframe_image.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Typical phone photos are 1-4 MB; the whole file must sit in PSRAM while decoding. */
#define JPEG_MAX_BYTES (4*1024*1024)
/* Decode-time downscale keeps the intermediate RGB565 frame at or under this size. */
#define MAX_DECODED_BYTES (1600u*1024u)
static const char *TAG="skyframe_jpeg";
static const uint8_t scale_div[]={1,2,4,8};

esp_err_t sf_jpeg_decode_fit(const uint8_t *data,size_t len,uint16_t **out_frame,
                             unsigned out_w,unsigned out_h,const char *name){
 if(!data||!len||!out_frame||!out_w||!out_h)return ESP_ERR_INVALID_ARG;
 *out_frame=NULL;
 esp_jpeg_image_cfg_t cfg={.indata=(uint8_t *)data,.indata_size=(uint32_t)len,.out_format=JPEG_IMAGE_FORMAT_RGB565,.out_scale=JPEG_IMAGE_SCALE_0};
 esp_jpeg_image_output_t info={0};esp_err_t e=esp_jpeg_get_image_info(&cfg,&info);
 if(e!=ESP_OK||!info.width||!info.height)return ESP_ERR_INVALID_SIZE;
 const uint32_t full_w=info.width,full_h=info.height;
 /* Largest reduction that still covers the output frame, then shrink further if
    the decoded frame would be too big or PSRAM is too fragmented to hold it. */
 int s=0;
 while(s<3&&full_w/scale_div[s+1]>=out_w&&full_h/scale_div[s+1]>=out_h)s++;
 while(s<3&&(size_t)(full_w/scale_div[s])*(full_h/scale_div[s])*2>MAX_DECODED_BYTES)s++;
 uint16_t *decoded=NULL;size_t bytes=0;
 for(;s<4;s++){
  bytes=(size_t)(full_w/scale_div[s])*(full_h/scale_div[s])*2;
  decoded=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(decoded)break;
 }
 if(!decoded)return ESP_ERR_NO_MEM;
 cfg.out_scale=(esp_jpeg_image_scale_t)s;cfg.outbuf=(uint8_t *)decoded;cfg.outbuf_size=bytes;
 e=esp_jpeg_decode(&cfg,&info);
 if(e!=ESP_OK){free(decoded);return e;}
 ESP_LOGI(TAG,"%s: %ux%u decoded at 1/%u -> %ux%u",name?name:"image",(unsigned)full_w,(unsigned)full_h,
          (unsigned)scale_div[s],(unsigned)info.width,(unsigned)info.height);
 uint16_t *out=heap_caps_malloc((size_t)out_w*out_h*2u,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!out||!sf_fit_rgb565(decoded,info.width,info.height,out,out_w,out_h)){
  free(decoded);free(out);return out?ESP_FAIL:ESP_ERR_NO_MEM;
 }
 free(decoded);*out_frame=out;return ESP_OK;
}

esp_err_t sf_jpeg_read_file(const char *path,uint8_t **data,size_t *len){
 if(!path||!data||!len)return ESP_ERR_INVALID_ARG;
 *data=NULL;*len=0;
 FILE *f=fopen(path,"rb");if(!f)return ESP_ERR_NOT_FOUND;
 if(fseek(f,0,SEEK_END)!=0){fclose(f);return ESP_FAIL;}
 long sz=ftell(f);if(sz<1||sz>JPEG_MAX_BYTES){fclose(f);return ESP_ERR_INVALID_SIZE;}
 rewind(f);
 uint8_t *raw=heap_caps_malloc((size_t)sz,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!raw){fclose(f);return ESP_ERR_NO_MEM;}
 if(fread(raw,1,(size_t)sz,f)!=(size_t)sz){free(raw);fclose(f);return ESP_FAIL;}
 fclose(f);
 *data=raw;*len=(size_t)sz;
 return ESP_OK;
}
esp_err_t sf_jpeg_load_fit(const char *path,uint16_t **out_frame,unsigned out_w,unsigned out_h){
 if (!path || !out_frame) {
  return ESP_ERR_INVALID_ARG;
 }
 *out_frame = NULL;
 uint8_t *raw=NULL;size_t len=0;
 esp_err_t e=sf_jpeg_read_file(path,&raw,&len);
 if(e!=ESP_OK)return e;
 e=sf_jpeg_decode_fit(raw,len,out_frame,out_w,out_h,path);
 free(raw);
 return e;
}
