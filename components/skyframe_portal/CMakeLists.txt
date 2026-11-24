idf_component_register(SRCS "skyframe_portal.c" "skyframe_wifi.c" "skyframe_photo_web.c" "skyframe_birthday_web.c" "skyframe_update_web.c" INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event esp_timer skyframe_config skyframe_storage skyframe_jpeg skyframe_engine skyframe_update app_update
                       PRIV_REQUIRES espressif__mdns
                       # Home-screen / tab icon (180x180 PNG of the web header's radar).
                       EMBED_FILES "apple-touch-icon.png")
