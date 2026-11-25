idf_component_register(SRCS "skyframe_net.c" "skyframe_urls.c" "skyframe_routes.c" INCLUDE_DIRS "include" REQUIRES skyframe_engine esp_http_client mbedtls espressif__cjson esp-tls)
