#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
/* All frames are PSRAM RGB565, center-cropped to the size asked for; the caller frees them.
   Photos prepared on a PC (tools/prepare_photos.py) decode fastest. */
/* Read a whole JPEG file (<= 4 MB) into a new PSRAM buffer the caller frees. Lets callers
   hold the SD card only for the read, then decode with sf_jpeg_decode_fit. */
esp_err_t sf_jpeg_read_file(const char *path,uint8_t **data,size_t *len);
/* Decode a JPEG file into an out_w x out_h frame. The caller must hold the card (sf_sd_begin). */
esp_err_t sf_jpeg_load_fit(const char *path,uint16_t **out_frame,unsigned out_w,unsigned out_h);
/* Decode a baseline JPEG held in memory into a new PSRAM RGB565 out_w x out_h frame
   (center-cropped, downscaled during decode when large). name is only for logging. */
esp_err_t sf_jpeg_decode_fit(const uint8_t *data,size_t len,uint16_t **out_frame,
                             unsigned out_w,unsigned out_h,const char *name);
