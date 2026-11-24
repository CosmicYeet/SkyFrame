#include "skyframe_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
/* Values stored as a versioned blob (sf_config_record_t). NVS writes are CRC-checked by
   NVS; invalid blobs fall back to safe defaults. Older versions are migrated by
   sf_config_decode so a firmware update never sends a configured device back to
   first-time setup. The old record stays until the next save, so going back to older
   firmware before saving anything still finds its settings. */
esp_err_t sf_config_nvs_init(void){
 esp_err_t e=nvs_flash_init();
 if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){
  ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();
 }return e;
}
esp_err_t sf_config_nvs_load(sf_config_t *out){
 if (!out) {
  return ESP_ERR_INVALID_ARG;
 }
 sf_config_defaults(out);
 nvs_handle_t h;esp_err_t e=nvs_open("skyframe",NVS_READONLY,&h);if(e!=ESP_OK)return e;
 unsigned char blob[sizeof(sf_config_record_t)];   /* the newest record is the largest */
 size_t size=sizeof(blob);e=nvs_get_blob(h,"settings",blob,&size);nvs_close(h);
 if(e!=ESP_OK)return ESP_ERR_INVALID_STATE;
 return sf_config_decode(blob,size,out)?ESP_OK:ESP_ERR_INVALID_STATE;
}
esp_err_t sf_config_nvs_save(const sf_config_t *cfg){
 if(!sf_config_valid(cfg))return ESP_ERR_INVALID_ARG;
 nvs_handle_t h;esp_err_t e=nvs_open("skyframe",NVS_READWRITE,&h);if(e!=ESP_OK)return e;
 sf_config_record_t r;
 memset(&r,0,sizeof(r));r.version=SF_CONFIG_RECORD_VERSION;r.data=*cfg;
 e=nvs_set_blob(h,"settings",&r,sizeof(r));
 if (e == ESP_OK) {
  e = nvs_commit(h);
 }
 nvs_close(h);
 return e;
}
