cmake_minimum_required(VERSION 3.16)
# The firmware version ("0.5.0") is in version.txt, so a release changes one line. Updates
# compare it (major.minor.patch) with the version the device runs.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
# Re-run CMake when version.txt changes, so a build never carries the previous version.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL tracks at most LV_INV_BUF_SIZE changed areas per frame and redraws the whole screen
# when more arrive. Moving planes (about 4-13 areas each) plus the sweep overflow the
# default 32, so allow more. Set for every component: lv_disp_t embeds arrays of this size.
idf_build_set_property(COMPILE_DEFINITIONS "LV_INV_BUF_SIZE=128" APPEND)
# LVGL allocates through skyframe_lvmem.c (CONFIG_LV_MEM_CUSTOM_INCLUDE="skyframe_lvmem.h"):
# big on-demand screens such as test mode go to PSRAM, leaving internal RAM to Wi-Fi and TLS.
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_ALLOC=sf_lv_malloc" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_REALLOC=sf_lv_realloc" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_FREE=sf_lv_free" APPEND)
idf_build_set_property(INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/components/skyframe_screens/include" APPEND)
project(SkyFrame)
