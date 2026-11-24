# SkyFrame simulator: SkyFrame's own code (screens, radar, rotation, test mode, network
# code with live data) built for Windows, with an SDL window as the round display.
# Build and run:  sim\run.ps1   (needs MSYS2 UCRT64: gcc, SDL2, curl, cmake, ninja)
cmake_minimum_required(VERSION 3.20)
project(skyframe_sim C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SDKCONFIG ${ROOT}/build/config/sdkconfig.h)
if(NOT EXISTS ${SDKCONFIG})
  message(FATAL_ERROR "Missing ${SDKCONFIG}: run 'idf.py build' once; the simulator uses the firmware's LVGL settings.")
endif()

find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# LVGL, exactly as configured for the firmware (no warnings: not our code).
file(GLOB_RECURSE LVGL_SRC ${ROOT}/managed_components/lvgl__lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${ROOT}/managed_components/lvgl__lvgl ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_compile_definitions(lvgl PUBLIC LV_CONF_KCONFIG_EXTERNAL_INCLUDE="sim_lv_conf.h"
                                       SIM_SDKCONFIG="${SDKCONFIG}" LV_INV_BUF_SIZE=128)
target_compile_options(lvgl PRIVATE -w -O2)

# Third-party pieces SkyFrame uses: cJSON and the JPEG decoder.
add_library(thirdparty STATIC
  ${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c
  ${ROOT}/managed_components/espressif__esp_jpeg/jpeg_decoder.c
  ${ROOT}/managed_components/espressif__esp_jpeg/jpeg_default_huffman_table.c
  ${ROOT}/managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c)
target_include_directories(thirdparty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${ROOT}/managed_components/espressif__cjson/cJSON
  ${ROOT}/managed_components/espressif__esp_jpeg/include
  ${ROOT}/managed_components/espressif__esp_jpeg/tjpgd)
# -Wno-error: the decoder's input callback says "unsigned int" where TJpgDec has size_t;
# identical on the ESP32 (32-bit) and harmless here.
target_compile_options(thirdparty PRIVATE -w -O2 -Wno-error=incompatible-pointer-types)
# TJpgDec's own header, but through the shim wrapper (see shim/tjpgd.h).
set_source_files_properties(${ROOT}/managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c
  PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/shim/tjpgd.h")

add_executable(skyframe_sim
  sim_platform.c sim_services.c
  ${ROOT}/main/main.c ${ROOT}/main/birthdays.c ${ROOT}/main/plane_library.c
  ${ROOT}/components/skyframe_engine/skyframe_engine.c
  ${ROOT}/components/skyframe_engine/skyframe_controller.c
  ${ROOT}/components/skyframe_engine/skyframe_today.c
  ${ROOT}/components/skyframe_config/skyframe_config.c
  ${ROOT}/components/skyframe_net/skyframe_net.c
  ${ROOT}/components/skyframe_net/skyframe_urls.c
  ${ROOT}/components/skyframe_ui/skyframe_ui.c
  ${ROOT}/components/skyframe_screens/skyframe_screens.c
  ${ROOT}/components/skyframe_screens/skyframe_photos.c
  ${ROOT}/components/skyframe_screens/skyframe_image.c
  ${ROOT}/components/skyframe_photoplayer/skyframe_photoplayer.c
  ${ROOT}/components/skyframe_jpeg/skyframe_jpeg.c)
# The shim folder comes first: its headers stand in for ESP-IDF's (and for skyframe_sd.h).
target_include_directories(skyframe_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${ROOT}/main
  ${ROOT}/components/skyframe_engine/include
  ${ROOT}/components/skyframe_config/include
  ${ROOT}/components/skyframe_net/include
  ${ROOT}/components/skyframe_ui/include
  ${ROOT}/components/skyframe_screens/include
  ${ROOT}/components/skyframe_photoplayer/include
  ${ROOT}/components/skyframe_jpeg/include
  ${ROOT}/components/skyframe_portal/include)
target_compile_definitions(skyframe_sim PRIVATE SIM_SD_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/sdcard")
target_compile_options(skyframe_sim PRIVATE -O2 -Wall -Wno-unused-function -Wno-format-truncation
                       -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/sim_compat.h)
target_link_libraries(skyframe_sim PRIVATE lvgl thirdparty SDL2::SDL2 CURL::libcurl Threads::Threads m)
