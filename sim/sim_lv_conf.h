/* LVGL settings for the simulator: exactly the firmware's, read from the sdkconfig.h that
   `idf.py build` generates (build/config/sdkconfig.h). */
#pragma once
#include SIM_SDKCONFIG
#include "sim_esp.h"   /* IRAM_ATTR and friends */
/* LVGL's own printf clashes with the Windows C library's names; the standard one is the same. */
#define CONFIG_LV_SPRINTF_CUSTOM 1
#define LV_SPRINTF_INCLUDE <stdio.h>
#define lv_snprintf snprintf
#define lv_vsnprintf vsnprintf
