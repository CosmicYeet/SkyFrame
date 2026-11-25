#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Offline aircraft photos on the microSD card, so a tapped plane shows a picture
   immediately instead of waiting for the network:

     /planes/types/<TYPE>.jpg   one representative photo per ICAO type code (e.g. B738.jpg),
                                prepared on a PC by tools/prepare_plane_photos.py
     /planes/saved/<ICAO>.rgb   photos of individual airframes, saved automatically the
                                first time each one is downloaded

   All images are SKYFRAME_PHOTO_W x SKYFRAME_PHOTO_H RGB565. Functions return NULL /
   an error when the card is absent or connected to a computer. Call from a worker task. */

/* Saved photo of this exact airframe (6-hex-digit ICAO address), or NULL. */
uint16_t *plane_library_load_saved(const char *icao);
/* Representative photo for an ICAO type designator, or NULL. */
uint16_t *plane_library_load_type(const char *type_code);
/* Save a downloaded airframe photo for next time. */
esp_err_t plane_library_save(const char *icao, const uint16_t *pixels);
/* A type seen in traffic: when the card has no photo for it, list it in /planes/missing.txt
   ("TYPE<tab>description", once each) for tools/prepare_plane_photos.py --missing. */
bool plane_library_note_type(const char *type_code, const char *description);   /* false: card busy, try later */
