idf_component_register(
    SRCS "main.c"
      "EXIO/TCA9554PWR.c"
      "LCD_Driver/ST7701S.c"
      "Touch_Driver/GT911.c"
      "Touch_Driver/esp_lcd_touch/esp_lcd_touch.c"
      "LVGL_Driver/LVGL_Driver.c"
      "I2C_Driver/I2C_Driver.c"
      "SD_Card/SD_MMC.c"
      "plane_library.c"
      "birthdays.c"
    INCLUDE_DIRS "." "EXIO" "LCD_Driver" "Touch_Driver" "Touch_Driver/esp_lcd_touch"
      "LVGL_Driver" "I2C_Driver" "SD_Card"
    REQUIRES lvgl__lvgl skyframe_engine skyframe_ui skyframe_screens skyframe_net
      skyframe_photoplayer skyframe_config skyframe_portal skyframe_jpeg skyframe_storage
      nvs_flash esp_wifi esp_event esp_netif esp_http_client esp_http_server
      fatfs esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc esp_lcd spi_flash mbedtls
      esp_timer esp_driver_sdmmc sdmmc esp_driver_tsens esp_adc esp_app_format
)
