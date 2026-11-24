/* LVGL's memory (LV_MEM_CUSTOM_INCLUDE; the allocator macros are set in the top CMakeLists).
   Small blocks normally go to internal RAM, which Wi-Fi and TLS need. Screens built on demand
   (test mode, setup) are large and not speed-critical: while they are built, LVGL's blocks
   go to PSRAM instead. Only the UI thread may switch it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

void *sf_lv_malloc(size_t size);
void *sf_lv_realloc(void *ptr, size_t size);
void sf_lv_free(void *ptr);
/* Nestable: true while building a big on-demand screen, false when done. */
void sf_lvmem_prefer_psram(bool on);
