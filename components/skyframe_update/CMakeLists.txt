idf_component_register(SRCS "skyframe_update.c" "skyframe_update_manifest.c" INCLUDE_DIRS "include"
                       REQUIRES skyframe_net app_update esp_https_ota esp_http_client espcoredump nvs_flash
                                mbedtls espressif__cjson esp_app_format esp_timer)
