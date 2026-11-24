#pragma once
#include "esp_err.h"
/* Start the slideshow source: a background task decodes photos from /photos (or
   /pictures) on the microSD card, rescanning whenever the card is (re)mounted.
   Call once from the LVGL task after sf_screens_init(). */
esp_err_t sf_photoplayer_start(void);
